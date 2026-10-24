from mesa_edit import replace

KGSL = 'src/freedreno/vulkan/tu_knl_kgsl.cc'
QUEUE_NEW = 'static int\nkgsl_submitqueue_new('
HELPER = """static int
dd_set_pwr_max_constraint(int fd, uint32_t context_id)
{
   struct kgsl_device_constraint_pwrlevel pwrlevel = {
      .level = KGSL_CONSTRAINT_PWR_MAX,
   };
   struct kgsl_device_constraint constraint = {
      .type = KGSL_CONSTRAINT_PWRLEVEL,
      .context_id = context_id,
      .data = (void *)&pwrlevel,
      .size = sizeof(pwrlevel),
   };
   struct kgsl_device_getproperty prop = {
      .type = KGSL_PROP_PWR_CONSTRAINT,
      .value = (void *)&constraint,
      .sizebytes = sizeof(constraint),
   };

   return safe_ioctl(fd, IOCTL_KGSL_SETPROPERTY, &prop);
}

"""

replace(
    KGSL,
    """              KGSL_CONTEXT_NO_GMEM_ALLOC |
              KGSL_CONTEXT_PREAMBLE,""",
    """              KGSL_CONTEXT_NO_GMEM_ALLOC |
              KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_PWR_CONSTRAINT,""",
    'KGSL_CONTEXT_PWR_CONSTRAINT context flag',
)

replace(KGSL, QUEUE_NEW, HELPER + QUEUE_NEW, 'PWR_MAX helper')

replace(
    KGSL,
    '   queue->msm_queue_id = req.drawctxt_id;\n\n   return 0;\n}',
    """   queue->msm_queue_id = req.drawctxt_id;

   if (dd_set_pwr_max_constraint(dev->physical_device->local_fd, req.drawctxt_id))
      mesa_logw("DD-Turnip: Failed to set initial PWR_MAX constraint: %s", strerror(errno));

   return 0;
}""",
    'initial PWR_MAX constraint',
)
replace(
    KGSL,
    '.flags = KGSL_CMDBATCH_SUBMIT_IB_LIST,',
    '.flags = KGSL_CMDBATCH_SUBMIT_IB_LIST | KGSL_CMDBATCH_PWR_CONSTRAINT,',
    'KGSL_CMDBATCH_PWR_CONSTRAINT submission flag',
)
replace(
    KGSL,
    '      timestamp = req.timestamp;\n   } else {',
    """      static uint32_t pwr_refresh_counter = 0;
      uint32_t count = p_atomic_inc_return(&pwr_refresh_counter);
      if (count % 1000 == 0 &&
          dd_set_pwr_max_constraint(queue->device->physical_device->local_fd,
                                    queue->msm_queue_id))
         mesa_logw("DD-Turnip: PWR_MAX refresh failed at submission %u: %s", count, strerror(errno));

      timestamp = req.timestamp;
   } else {""",
    'PWR_MAX refresh every 1000 submissions',
)
