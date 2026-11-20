// External semaphore helpers.
#pragma once
#include "vkutil.hpp"

inline VkSemaphore make_sem(Ctx &c, bool timeline, VkExternalSemaphoreHandleTypeFlags export_types, uint64_t init = 0)
{
	VkSemaphoreTypeCreateInfo t{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	t.semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;
	t.initialValue = timeline ? init : 0;
	VkExportSemaphoreCreateInfo e{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	e.pNext = &t;
	e.handleTypes = export_types;
	VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	ci.pNext = export_types ? (void *)&e : (void *)&t;
	VkSemaphore s;
	VK_CHECK(vkCreateSemaphore(c.dev, &ci, nullptr, &s));
	return s;
}

inline VkResult sem_export(Ctx &c, VkSemaphore s, VkExternalSemaphoreHandleTypeFlagBits type, int *fd)
{
	VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	gi.semaphore = s;
	gi.handleType = type;
	*fd = -1;
	return c.dfn<PFN_vkGetSemaphoreFdKHR>("vkGetSemaphoreFdKHR")(c.dev, &gi, fd);
}

// Takes ownership of fd on success.
inline VkResult sem_import(Ctx &c, VkSemaphore s, VkExternalSemaphoreHandleTypeFlagBits type, int fd,
			   bool temporary)
{
	VkImportSemaphoreFdInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
	ii.semaphore = s;
	ii.handleType = type;
	ii.fd = fd;
	ii.flags = temporary ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : 0;
	return c.dfn<PFN_vkImportSemaphoreFdKHR>("vkImportSemaphoreFdKHR")(c.dev, &ii);
}

inline VkSemaphoreSubmitInfo sem_info(VkSemaphore s, uint64_t value = 0,
				      VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)
{
	VkSemaphoreSubmitInfo i{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	i.semaphore = s;
	i.value = value;
	i.stageMask = stage;
	return i;
}

inline VkResult host_wait(Ctx &c, VkSemaphore s, uint64_t value, uint64_t timeout_ns)
{
	VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &s;
	wi.pValues = &value;
	return vkWaitSemaphores(c.dev, &wi, timeout_ns);
}

inline void host_signal(Ctx &c, VkSemaphore s, uint64_t value)
{
	VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
	si.semaphore = s;
	si.value = value;
	VK_CHECK(vkSignalSemaphore(c.dev, &si));
}

inline uint64_t sem_value(Ctx &c, VkSemaphore s)
{
	uint64_t v = 0;
	VK_CHECK(vkGetSemaphoreCounterValue(c.dev, s, &v));
	return v;
}

// Empty submit that waits/signals (no command buffer).
inline void empty_submit(Ctx &c, std::vector<VkSemaphoreSubmitInfo> waits, std::vector<VkSemaphoreSubmitInfo> sigs)
{
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.waitSemaphoreInfoCount = (uint32_t)waits.size();
	si.pWaitSemaphoreInfos = waits.data();
	si.signalSemaphoreInfoCount = (uint32_t)sigs.size();
	si.pSignalSemaphoreInfos = sigs.data();
	VK_CHECK(vkQueueSubmit2(c.q, 1, &si, VK_NULL_HANDLE));
}
