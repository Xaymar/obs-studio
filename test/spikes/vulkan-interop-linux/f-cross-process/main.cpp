// Spike F: cross-process sharing. The parent exports a dmabuf (BGRA, driver-chosen modifier), an OPAQUE_FD image
// (optimal tiling), a timeline semaphore (OPAQUE_FD) and a pending sync_file, and sends the fds over a Unix socket
// with SCM_RIGHTS to a child process (fork + exec of this binary). The child imports everything into its own
// VkInstance/VkDevice, GPU-waits on the timeline, verifies both images, writes a new pattern into the dmabuf and
// GPU-signals the timeline back; the parent then verifies the child's pixels.
#include "dmabuf.hpp"
#include "sem.hpp"

#include <sys/socket.h>
#include <sys/wait.h>

static const uint32_t W = 256, H = 128;
static const VkImageUsageFlags USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

struct Msg {
	uint64_t modifier;
	uint32_t nplanes;
	PlaneLayout planes[4];
	uint64_t opaque_size;
	uint8_t device_uuid[VK_UUID_SIZE], driver_uuid[VK_UUID_SIZE];
	uint64_t wait_value, signal_value;
};

static void send_fds(int sock, const void *data, size_t len, const int *fds, int nfds)
{
	iovec iov{(void *)data, len};
	char ctrl[CMSG_SPACE(sizeof(int) * 8)] = {};
	msghdr m{};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctrl;
	m.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
	cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
	memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
	if (sendmsg(sock, &m, 0) < 0)
		perror("sendmsg");
}

static int recv_fds(int sock, void *data, size_t len, int *fds, int max)
{
	iovec iov{data, len};
	char ctrl[CMSG_SPACE(sizeof(int) * 8)] = {};
	msghdr m{};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctrl;
	m.msg_controllen = sizeof ctrl;
	if (recvmsg(sock, &m, MSG_CMSG_CLOEXEC) <= 0)
		return -1;
	int n = 0;
	for (cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
			memcpy(fds, CMSG_DATA(c), sizeof(int) * std::min(n, max));
		}
	return n;
}

static void uuids(Ctx &c, uint8_t *dev, uint8_t *drv)
{
	VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p.pNext = &id;
	vkGetPhysicalDeviceProperties2(c.phys, &p);
	memcpy(dev, id.deviceUUID, VK_UUID_SIZE);
	memcpy(drv, id.driverUUID, VK_UUID_SIZE);
}

static VkImage opaque_image(Ctx &c)
{
	VkExternalMemoryImageCreateInfo ei{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ei.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &ei;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_B8G8R8A8_UNORM;
	ici.extent = {W, H, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = USAGE;
	VkImage img;
	VK_CHECK(vkCreateImage(c.dev, &ici, nullptr, &img));
	return img;
}

static DmaImage opaque_alloc(Ctx &c, int import_fd, uint64_t import_size)
{
	DmaImage d;
	d.fmt = VK_FORMAT_B8G8R8A8_UNORM;
	d.w = W;
	d.h = H;
	d.img = opaque_image(c);
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(c.dev, d.img, &mr);
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = d.img;
	VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	ex.pNext = &ded;
	ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.pNext = &ded;
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	imp.fd = import_fd;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = import_fd >= 0 ? (void *)&imp : (void *)&ex;
	mai.allocationSize = import_fd >= 0 ? import_size : mr.size;
	mai.memoryTypeIndex = c.mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(c.dev, &mai, nullptr, &d.mem));
	VK_CHECK(vkBindImageMemory(c.dev, d.img, d.mem, 0));
	return d;
}

static std::vector<const char *> exts() { return dmabuf_exts(); }

static int child(int sock)
{
	Ctx c;
	c.init(exts());
	printf("[child pid %d] ", getpid());
	print_driver(c);
	Msg m;
	int fds[8];
	int n = recv_fds(sock, &m, sizeof m, fds, 8);
	check(n == 4, "[child] received %d fds over SCM_RIGHTS", n);
	if (n != 4)
		return 1;
	uint8_t dev[VK_UUID_SIZE], drv[VK_UUID_SIZE];
	uuids(c, dev, drv);
	check(!memcmp(dev, m.device_uuid, VK_UUID_SIZE) && !memcmp(drv, m.driver_uuid, VK_UUID_SIZE),
	      "[child] device/driver UUID match (required for OPAQUE_FD)");

	DmaImage dma;
	std::string err;
	bool ok = import_dmabuf(c, dma, fds[0], VK_FORMAT_B8G8R8A8_UNORM, W, H, m.modifier,
				std::vector<PlaneLayout>(m.planes, m.planes + m.nplanes), USAGE, &err);
	close(fds[0]);
	check(ok, "[child] dmabuf imported (%s)", ok ? "ok" : err.c_str());
	DmaImage opq = opaque_alloc(c, fds[1], m.opaque_size); // fd owned by Vulkan now
	VkSemaphore tl = make_sem(c, true, 0);
	VkResult r = sem_import(c, tl, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, fds[2], false);
	check(r == VK_SUCCESS, "[child] timeline semaphore imported (%s)", vk_result_str(r));
	VkSemaphore sf = make_sem(c, false, 0);
	r = sem_import(c, sf, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, fds[3], true);
	check(r == VK_SUCCESS, "[child] sync_file imported (%s)", vk_result_str(r));
	printf("[child] timeline value at import: %llu (parent's work still gated)\n",
	       (unsigned long long)sem_value(c, tl));

	// The reads wait on the GPU for the parent's timeline value and on the sync_file.
	VkSemaphoreSubmitInfo w1 = sem_info(tl, m.wait_value);
	size_t bad = ok ? acquire_and_verify(c, dma, 3, &w1) : 1;
	check(bad == 0, "[child] dmabuf pixels written by the parent (%zu bad bytes)", bad);
	VkSemaphoreSubmitInfo w2 = sem_info(sf);
	bad = acquire_and_verify(c, opq, 4, &w2, VK_QUEUE_FAMILY_EXTERNAL);
	check(bad == 0, "[child] OPAQUE_FD image pixels written by the parent (%zu bad bytes)", bad);
	// Write back into the dmabuf and signal the parent's timeline from the GPU.
	if (ok) {
		VkSemaphoreSubmitInfo s = sem_info(tl, m.signal_value);
		upload_and_release(c, dma, 9, &s);
		VK_CHECK(vkQueueWaitIdle(c.q));
	}
	int result = g_fail;
	send(sock, &result, sizeof result, 0);
	vkDestroySemaphore(c.dev, tl, nullptr);
	vkDestroySemaphore(c.dev, sf, nullptr);
	destroy_dma_image(c, opq);
	if (ok)
		destroy_dma_image(c, dma);
	int rc = finish(c, "spike F child");
	c.destroy();
	return rc;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--child"))
		return child(atoi(argv[2]));
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) {
		perror("socketpair");
		return 1;
	}
	// fork before any Vulkan call, then exec: the child is a fresh process with its own driver state
	pid_t pid = fork();
	if (pid == 0) {
		close(sv[0]);
		char fd[16];
		snprintf(fd, sizeof fd, "%d", sv[1]);
		execl("/proc/self/exe", argv[0], "--child", fd, (char *)nullptr);
		_exit(127);
	}
	close(sv[1]);
	int sock = sv[0];

	Ctx c;
	c.init(exts());
	printf("[parent pid %d] ", getpid());
	print_driver(c);
	std::vector<uint64_t> mods;
	for (auto &m : query_modifiers(c, VK_FORMAT_B8G8R8A8_UNORM, USAGE, W, H))
		if (m.importable && m.exportable)
			mods.push_back(m.mod);
	DmaImage dma = create_exportable(c, VK_FORMAT_B8G8R8A8_UNORM, W, H, USAGE, mods);
	DmaImage opq = opaque_alloc(c, -1, 0);
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(c.dev, opq.img, &mr);
	VkSemaphore tl = make_sem(c, true, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
	VkSemaphore bin = make_sem(c, false, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
	VkSemaphore gate = make_sem(c, true, 0);

	// Gate all parent GPU work so the child imports while it is still pending.
	empty_submit(c, {sem_info(gate, 1)}, {});
	VkSemaphoreSubmitInfo s1 = sem_info(tl, 1);
	upload_and_release(c, dma, 3, &s1);
	VkSemaphoreSubmitInfo s2 = sem_info(bin);
	upload_and_release(c, opq, 4, &s2, VK_QUEUE_FAMILY_EXTERNAL);

	Msg m{};
	m.modifier = dma.modifier;
	m.nplanes = (uint32_t)dma.planes.size();
	for (uint32_t i = 0; i < m.nplanes; i++)
		m.planes[i] = dma.planes[i];
	m.opaque_size = mr.size;
	uuids(c, m.device_uuid, m.driver_uuid);
	m.wait_value = 1;
	m.signal_value = 2;
	int fds[4] = {export_fd(c, dma.mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT),
		      export_fd(c, opq.mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT), -1, -1};
	VK_CHECK(sem_export(c, tl, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, &fds[2]));
	VK_CHECK(sem_export(c, bin, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, &fds[3]));
	send_fds(sock, &m, sizeof m, fds, 4);
	for (int fd : fds)
		close(fd);
	printf("[parent] sent dmabuf (modifier 0x%llx), opaque image (%llu bytes), timeline, sync_file\n",
	       (unsigned long long)m.modifier, (unsigned long long)m.opaque_size);
	usleep(200 * 1000);
	check(sem_value(c, tl) == 0, "[parent] nothing signalled while gated");
	host_signal(c, gate, 1);

	VkResult r = host_wait(c, tl, 2, 10ull * 1000000000ull);
	check(r == VK_SUCCESS, "[parent] child's GPU signal of value 2 received (%s)", vk_result_str(r));
	int child_fail = -1;
	recv(sock, &child_fail, sizeof child_fail, 0);
	VK_CHECK(vkQueueWaitIdle(c.q));
	size_t bad = acquire_and_verify(c, dma, 9);
	check(bad == 0, "[parent] dmabuf pixels written back by the child (%zu bad bytes)", bad);
	int status = 0;
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && child_fail == 0, "[parent] child exited cleanly (%d, %d failures)",
	      WEXITSTATUS(status), child_fail);
	for (auto s : {tl, bin, gate})
		vkDestroySemaphore(c.dev, s, nullptr);
	destroy_dma_image(c, dma);
	destroy_dma_image(c, opq);
	int rc = finish(c, "spike F");
	c.destroy();
	return rc;
}
