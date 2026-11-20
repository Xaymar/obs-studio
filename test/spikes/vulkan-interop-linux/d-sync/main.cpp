// Spike D: timeline semaphores exported as OPAQUE_FD / SYNC_FD between two VkDevices, and DRM syncobj timeline
// points (PipeWire explicit sync: acquire/release syncobj + point) bridged into Vulkan and back.
#include "sem.hpp"

#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <xf86drm.h>

static const uint64_t SEC = 1000000000ull;

static void print_caps(Ctx &c)
{
	printf("--- external semaphore properties\n");
	for (int timeline = 0; timeline < 2; timeline++)
		for (auto t : {VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT}) {
			VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
			ti.semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;
			VkPhysicalDeviceExternalSemaphoreInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
			ei.pNext = &ti;
			ei.handleType = t;
			VkExternalSemaphoreProperties p{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
			vkGetPhysicalDeviceExternalSemaphoreProperties(c.phys, &ei, &p);
			printf("  %-8s %-9s export %s import %s\n", timeline ? "timeline" : "binary",
			       t == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT ? "OPAQUE_FD" : "SYNC_FD",
			       (p.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) ? "y" : "n",
			       (p.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ? "y" : "n");
		}
}

static void timeline_opaque(Ctx &a, Ctx &b, int drm)
{
	printf("--- timeline semaphore, OPAQUE_FD, device A <-> device B\n");
	VkSemaphore sa = make_sem(a, true, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
	int fd;
	VkResult r = sem_export(a, sa, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, &fd);
	check(r == VK_SUCCESS, "export timeline OPAQUE_FD (%s)", vk_result_str(r));
	if (r != VK_SUCCESS)
		return;
	if (drm >= 0) {
		uint32_t h = 0;
		int e = drmSyncobjFDToHandle(drm, fd, &h);
		printf("  is the OPAQUE_FD a DRM syncobj? drmSyncobjFDToHandle: %s\n", e ? strerror(-e) : "yes");
		if (!e)
			drmSyncobjDestroy(drm, h);
	}
	VkSemaphore sb = make_sem(b, true, 0);
	r = sem_import(b, sb, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, fd, false);
	check(r == VK_SUCCESS, "import timeline OPAQUE_FD into device B (%s)", vk_result_str(r));
	if (r != VK_SUCCESS) {
		close(fd);
		return;
	}
	host_signal(a, sa, 5);
	check(host_wait(b, sb, 5, SEC) == VK_SUCCESS && sem_value(b, sb) == 5, "host signal 5 on A seen by B");
	// GPU: B waits for 10 then signals 11; A host-signals 10 later; A waits for 11.
	empty_submit(b, {sem_info(sb, 10)}, {sem_info(sb, 11)});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	check(sem_value(a, sa) == 5, "B's GPU submit blocked on 10 (value still %llu)", (unsigned long long)sem_value(a, sa));
	empty_submit(a, {}, {sem_info(sa, 10)});
	check(host_wait(a, sa, 11, SEC) == VK_SUCCESS, "A GPU-signals 10 -> B GPU waits, signals 11 -> A sees 11");
	vkQueueWaitIdle(a.q);
	vkQueueWaitIdle(b.q);
	vkDestroySemaphore(b.dev, sb, nullptr);
	vkDestroySemaphore(a.dev, sa, nullptr);
}

static void sync_fd(Ctx &a, Ctx &b)
{
	printf("--- SYNC_FD\n");
	VkSemaphore tl = make_sem(a, true, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
	int fd;
	empty_submit(a, {}, {sem_info(tl, 1)});
	VkResult r = sem_export(a, tl, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, &fd);
	printf("  timeline semaphore SYNC_FD export: %s (properties say not exportable; the spec only allows binary)\n",
	       vk_result_str(r));
	if (fd >= 0)
		close(fd);
	vkQueueWaitIdle(a.q);
	vkDestroySemaphore(a.dev, tl, nullptr);

	// binary: A's submit waits on a host-controlled timeline (gate), signals binary; export SYNC_FD while pending.
	VkSemaphore gate = make_sem(a, true, 0);
	VkSemaphore bin = make_sem(a, false, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
	empty_submit(a, {sem_info(gate, 1)}, {sem_info(bin)});
	r = sem_export(a, bin, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, &fd);
	check(r == VK_SUCCESS && fd >= 0, "binary SYNC_FD export of a pending signal (%s, fd %d)", vk_result_str(r), fd);
	if (r != VK_SUCCESS || fd < 0) {
		host_signal(a, gate, 1);
		vkQueueWaitIdle(a.q);
		return;
	}
	pollfd p{fd, POLLIN, 0};
	int pr = poll(&p, 1, 50);
	check(pr == 0, "sync_file not signalled while the gate is closed (poll %d)", pr);
	int fd2 = dup(fd);
	// device B waits on the sync_file (temporary import into a binary semaphore) and signals a timeline.
	VkSemaphore bwait = make_sem(b, false, 0);
	VkSemaphore bdone = make_sem(b, true, 0);
	r = sem_import(b, bwait, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, fd2, true);
	check(r == VK_SUCCESS, "import SYNC_FD (temporary) into device B (%s)", vk_result_str(r));
	empty_submit(b, {sem_info(bwait)}, {sem_info(bdone, 1)});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	check(sem_value(b, bdone) == 0, "B blocked on the sync_file");
	host_signal(a, gate, 1);
	pr = poll(&p, 1, 1000);
	check(pr == 1, "sync_file signalled after the gate opens (poll %d)", pr);
	check(host_wait(b, bdone, 1, SEC) == VK_SUCCESS, "B's submit ran after the sync_file signalled");
	close(fd);
	vkQueueWaitIdle(a.q);
	vkQueueWaitIdle(b.q);
	for (auto s : {gate, bin})
		vkDestroySemaphore(a.dev, s, nullptr);
	for (auto s : {bwait, bdone})
		vkDestroySemaphore(b.dev, s, nullptr);
}

// PipeWire explicit sync: the buffer carries an acquire syncobj + point (producer signals) and a release syncobj +
// point (consumer signals). Here Vulkan is the consumer: wait for acquire, render, signal release.
static void drm_syncobj(Ctx &c, int drm)
{
	printf("--- DRM syncobj timeline (PipeWire explicit sync shape)\n");
	uint64_t cap = 0;
	int e = drmGetCap(drm, DRM_CAP_SYNCOBJ_TIMELINE, &cap);
	check(e == 0 && cap, "DRM_CAP_SYNCOBJ_TIMELINE on renderD128 (%s, %llu)", e ? strerror(errno) : "ok",
	      (unsigned long long)cap);
	if (e || !cap)
		return;
	uint32_t acquire = 0, release = 0;
	check(drmSyncobjCreate(drm, 0, &acquire) == 0 && drmSyncobjCreate(drm, 0, &release) == 0, "drmSyncobjCreate x2");
	const uint64_t ACQ = 3, REL = 4;

	// Path 1: import the acquire syncobj fd directly as a Vulkan timeline semaphore (OPAQUE_FD).
	{
		int fd = -1;
		drmSyncobjHandleToFD(drm, acquire, &fd);
		VkSemaphore s = make_sem(c, true, 0);
		VkResult r = sem_import(c, s, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, fd, false);
		printf("  direct: import syncobj fd as timeline OPAQUE_FD: %s\n", vk_result_str(r));
		if (r == VK_SUCCESS) {
			uint64_t p = 1;
			drmSyncobjTimelineSignal(drm, &acquire, &p, 1);
			check(host_wait(c, s, 1, SEC) == VK_SUCCESS, "direct: drmSyncobjTimelineSignal(1) seen by Vulkan");
			host_signal(c, s, 2);
			uint64_t q = 0;
			drmSyncobjQuery(drm, &acquire, &q, 1);
			check(q == 2, "direct: Vulkan host signal 2 seen by drmSyncobjQuery (%llu)", (unsigned long long)q);
		} else {
			close(fd);
			printf("  (direct import not supported: use the sync_file bridge)\n");
		}
		vkDestroySemaphore(c.dev, s, nullptr);
	}

	// Path 2: sync_file bridge, as compositors do it with any driver.
	// Producer (a thread standing in for the PipeWire client) signals the acquire point later.
	std::thread producer([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		uint64_t p = ACQ;
		drmSyncobjTimelineSignal(drm, &acquire, &p, 1);
	});
	// Consumer: wait until the acquire point has a fence (WAIT_AVAILABLE), move it into a binary syncobj, export
	// a sync_file, import into Vulkan as a temporary binary semaphore wait.
	uint64_t p = ACQ;
	e = drmSyncobjTimelineWait(drm, &acquire, &p, 1, INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, nullptr);
	check(e == 0, "acquire point %llu available (%s)", (unsigned long long)ACQ, e ? strerror(-e) : "ok");
	uint32_t tmp = 0;
	drmSyncobjCreate(drm, 0, &tmp);
	e = drmSyncobjTransfer(drm, tmp, 0, acquire, ACQ, 0);
	int acq_fd = -1;
	if (!e)
		e = drmSyncobjExportSyncFile(drm, tmp, &acq_fd);
	check(e == 0 && acq_fd >= 0, "acquire point -> sync_file (%s)", e ? strerror(-e) : "ok");
	VkSemaphore vacq = make_sem(c, false, 0);
	VkSemaphore vrel = make_sem(c, false, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
	VkSemaphore done = make_sem(c, true, 0);
	VkResult r = sem_import(c, vacq, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, acq_fd, true);
	check(r == VK_SUCCESS, "acquire sync_file imported into Vulkan (%s)", vk_result_str(r));
	// "Render": an empty submit waiting on acquire, signalling the exportable binary release semaphore.
	empty_submit(c, {sem_info(vacq)}, {sem_info(vrel), sem_info(done, 1)});
	int rel_fd = -1;
	r = sem_export(c, vrel, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, &rel_fd);
	check(r == VK_SUCCESS, "release semaphore exported as sync_file (%s)", vk_result_str(r));
	// sync_file -> release point (transfer from a binary syncobj into the timeline point)
	uint32_t tmp2 = 0;
	drmSyncobjCreate(drm, 0, &tmp2);
	e = drmSyncobjImportSyncFile(drm, tmp2, rel_fd);
	if (!e)
		e = drmSyncobjTransfer(drm, release, REL, tmp2, 0, 0);
	check(e == 0, "release sync_file -> syncobj point %llu (%s)", (unsigned long long)REL, e ? strerror(-e) : "ok");
	p = REL;
	e = drmSyncobjTimelineWait(drm, &release, &p, 1, (int64_t)(SEC * 2), DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
				   nullptr);
	// drmSyncobjTimelineWait takes an absolute CLOCK_MONOTONIC timeout; recompute on failure for a clear message
	if (e) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t abs = (int64_t)ts.tv_sec * SEC + ts.tv_nsec + 2 * SEC;
		e = drmSyncobjTimelineWait(drm, &release, &p, 1, abs, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, nullptr);
	}
	check(e == 0, "release point %llu signalled by the GPU (%s)", (unsigned long long)REL, e ? strerror(-e) : "ok");
	check(host_wait(c, done, 1, SEC) == VK_SUCCESS, "Vulkan submit completed after the acquire point");
	producer.join();
	if (rel_fd >= 0)
		close(rel_fd);
	vkQueueWaitIdle(c.q);
	for (auto s : {vacq, vrel, done})
		vkDestroySemaphore(c.dev, s, nullptr);
	for (auto h : {acquire, release, tmp, tmp2})
		drmSyncobjDestroy(drm, h);
}

int main()
{
	std::vector<const char *> exts = {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
	Ctx a, b;
	a.init(exts);
	b.init(exts);
	print_driver(a);
	print_caps(a);
	int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (drm < 0)
		printf("  /dev/dri/renderD128: %s\n", strerror(errno));
	timeline_opaque(a, b, drm);
	sync_fd(a, b);
	if (drm >= 0) {
		drm_syncobj(a, drm);
		close(drm);
	} else {
		check(false, "DRM render node for syncobj tests");
	}
	int rc = finish(a, "spike D");
	b.destroy();
	a.destroy();
	return rc;
}
