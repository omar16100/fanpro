/*
 * fanpro - real AppleSMC backend.
 *
 * Opens the AppleSMC user client and issues the single struct method every
 * SMC tool since smcFanControl has used.  This file is deliberately thin:
 * all protocol knowledge lives in smc.c, all encoding in smc_codec.c, so the
 * only thing that cannot be tested without hardware is these ~80 lines.
 *
 * Reads work as any user.  Writes are gated by the firmware against uid and
 * come back as kIOReturnNotPrivileged (0xe00002c2) when not root.
 */
#include "fanpro/smc.h"

#include "fanpro/log.h"

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stddef.h>
#include <string.h>

/* AppleSMC exposes one user client type and one selector for key traffic. */
#define SMC_CONNECTION_TYPE 0
#define SMC_SELECTOR_YPC    2

typedef struct {
	io_connect_t conn;
} iokit_ctx_t;

static iokit_ctx_t g_ctx = { .conn = IO_OBJECT_NULL };

static int
iokit_open(fanpro_smc_backend_t *be)
{
	iokit_ctx_t *ctx = (iokit_ctx_t *)be->ctx;
	io_service_t service;
	kern_return_t kr;

	if (ctx->conn != IO_OBJECT_NULL)
		return 0;

	service = IOServiceGetMatchingService(kIOMainPortDefault,
	                                      IOServiceMatching("AppleSMC"));
	if (service == IO_OBJECT_NULL) {
		FANPRO_ERROR("smc.iokit.no_service", "match=AppleSMC");
		return -1;
	}

	kr = IOServiceOpen(service, mach_task_self(), SMC_CONNECTION_TYPE,
	                   &ctx->conn);
	IOObjectRelease(service);

	if (kr != KERN_SUCCESS) {
		FANPRO_ERROR("smc.iokit.open_failed", "kr=0x%08x", kr);
		ctx->conn = IO_OBJECT_NULL;
		return -2;
	}
	return 0;
}

static void
iokit_close(fanpro_smc_backend_t *be)
{
	iokit_ctx_t *ctx = (iokit_ctx_t *)be->ctx;

	if (ctx->conn != IO_OBJECT_NULL) {
		IOServiceClose(ctx->conn);
		ctx->conn = IO_OBJECT_NULL;
	}
}

static int
iokit_call(fanpro_smc_backend_t *be, const fanpro_smc_key_data_t *in,
           fanpro_smc_key_data_t *out, uint32_t *io_err)
{
	iokit_ctx_t *ctx = (iokit_ctx_t *)be->ctx;
	size_t out_size = sizeof(*out);
	kern_return_t kr;

	if (ctx->conn == IO_OBJECT_NULL)
		return -1;

	kr = IOConnectCallStructMethod(ctx->conn, SMC_SELECTOR_YPC, in,
	                               sizeof(*in), out, &out_size);
	if (io_err != NULL)
		*io_err = (uint32_t)kr;

	if (kr != KERN_SUCCESS)
		return -2;
	if (out_size != sizeof(*out))
		return -3;
	return 0;
}

static fanpro_smc_backend_t g_backend = {
	.name = "iokit",
	.ctx = &g_ctx,
	.open = iokit_open,
	.close = iokit_close,
	.call = iokit_call,
};

fanpro_smc_backend_t *
fanpro_smc_backend_iokit(void)
{
	return &g_backend;
}
