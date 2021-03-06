#define _GNU_SOURCE

#include <libmemcached/memcached.h>
#include <libmemcached/util.h>

#include "vrt.h"
#include "cache/cache.h"

#include "vcc_if.h"

#include <string.h>
#include <time.h>


#define POOL_MAX_CONN_STR     "40"
#define POOL_MAX_CONN_PREFIX  "--POOL-MAX="
#define POOL_MAX_CONN_PARAM   " " POOL_MAX_CONN_PREFIX POOL_MAX_CONN_STR
#define POOL_TIMEOUT_MSEC     3000
#define POOL_ERROR_INT        -1
#define POOL_ERROR_STRING     NULL


typedef struct
{
	memcached_pool_st  *pool;
	long                pool_timeout_msec;
	int                 error_int;
	char               *error_str;
	char                error_str_value[128];
}
vmod_mc_vcl_settings;


static void free_mc_vcl_settings(void *data)
{
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)data;

	memcached_pool_destroy(settings->pool);

	free(settings);
}

int init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
	vmod_mc_vcl_settings *settings;

	settings=calloc(1, sizeof(vmod_mc_vcl_settings));

	AN(settings);

	settings->pool_timeout_msec = POOL_TIMEOUT_MSEC;
	settings->error_int = POOL_ERROR_INT;
	settings->error_str = POOL_ERROR_STRING;

	priv->priv=settings;
	priv->free=free_mc_vcl_settings;

	return 0;
}

static memcached_st *get_memcached(const struct vrt_ctx *ctx, vmod_mc_vcl_settings *settings)
{
	memcached_return_t rc;
	memcached_st *mc;
	struct timespec wait;

	AN(settings->pool);

	wait.tv_nsec = 1000 * 1000 * (settings->pool_timeout_msec % 1000);
	wait.tv_sec = settings->pool_timeout_msec / 1000;

	mc = memcached_pool_fetch(settings->pool, &wait, &rc);

	if(rc == MEMCACHED_SUCCESS)
	{
		return mc;
	}

	return NULL;
}

static void release_memcached(const struct vrt_ctx *ctx, vmod_mc_vcl_settings *settings, memcached_st *mc)
{
	memcached_pool_release(settings->pool, mc);
}

VCL_VOID vmod_servers(const struct vrt_ctx *ctx, struct vmod_priv *priv, VCL_STRING config)
{
	char error_buf[256];
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;

	AZ(settings->pool);

	if(strcasestr(config, POOL_MAX_CONN_PREFIX))
	{
		settings->pool = memcached_pool(config, strlen(config));

		VSL(SLT_Debug, 0, "memcached pool config '%s'", config);
	}
	else
	{
		size_t pool_len = strlen(config) + strlen(POOL_MAX_CONN_PARAM);
		char *pool_str = malloc(pool_len + 1);

		strcpy(pool_str, config);
		strcat(pool_str, POOL_MAX_CONN_PARAM);

		settings->pool = memcached_pool(pool_str, pool_len);

		VSL(SLT_Debug, 0, "memcached pool config '%s'", pool_str);

		free(pool_str);
	}

	if(!settings->pool)
	{
		libmemcached_check_configuration(config, strlen(config), error_buf, sizeof(error_buf));
		VSL(SLT_Error, 0, "memcached servers() error");
		VSL(SLT_Error, 0, "%s", error_buf);
		AN(settings->pool);
	}
}

VCL_VOID vmod_error_string(const struct vrt_ctx *ctx, struct vmod_priv *priv, VCL_STRING string)
{
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;

	strncpy(settings->error_str_value, string, sizeof(settings->error_str_value));
	settings->error_str_value[sizeof(settings->error_str_value) - 1] = '\0';

	settings->error_str = settings->error_str_value;
}

VCL_VOID vmod_pool_timeout_msec(const struct vrt_ctx *ctx, struct vmod_priv *priv, VCL_INT timeout)
{
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;

	settings->pool_timeout_msec = timeout;
}

VCL_VOID vmod_set(const struct vrt_ctx *ctx, struct vmod_priv *priv, VCL_STRING key,
	VCL_STRING value, VCL_INT expiration, VCL_INT flags)
{
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;
	memcached_st *mc = get_memcached(ctx, settings);

	if(mc)
	{
		memcached_set(mc, key, strlen(key), value, strlen(value), expiration, flags);
		release_memcached(ctx, settings, mc);
	}
}

VCL_STRING vmod_get(const struct vrt_ctx *ctx, struct vmod_priv *priv, VCL_STRING key)
{
	size_t len;
	uint32_t flags;
	memcached_return rc;
	char *p, *value;
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;
	memcached_st *mc = get_memcached(ctx, settings);

	if (!mc)
	{
		return settings->error_str;
	}

	value = memcached_get(mc, key, strlen(key), &len, &flags, &rc);

	release_memcached(ctx, settings, mc);

	if(rc || !value)
	{
		return settings->error_str;
	}

	p = WS_Copy(ctx->ws, value, -1);

	free(value);

	return p;
}

VCL_INT vmod_incr(const struct vrt_ctx *ctx, struct vmod_priv *priv, VCL_STRING key, VCL_INT offset)
{
	memcached_return_t rc;
	uint64_t value = 0;
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;
	memcached_st *mc = get_memcached(ctx, settings);

	if (!mc)
	{
		return settings->error_int;
	}

	memcached_increment(mc, key, strlen(key), offset, &value);

	rc = memcached_last_error(mc);

	release_memcached(ctx, settings, mc);

	if(rc)
	{
		return settings->error_int;
	}

	return (VCL_INT)value;
}

VCL_INT vmod_decr(const struct vrt_ctx *ctx, struct vmod_priv *priv, VCL_STRING key, VCL_INT offset)
{
	memcached_return_t rc;
	uint64_t value = 0;
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;
	memcached_st *mc = get_memcached(ctx, settings);

	if (!mc)
	{
		return settings->error_int;
	}

	memcached_decrement(mc, key, strlen(key), offset, &value);

	rc = memcached_last_error(mc);

	release_memcached(ctx, settings, mc);

	if(rc)
	{
		return settings->error_int;
	}

	return (VCL_INT)value;
}

VCL_INT vmod_incr_set(const struct vrt_ctx *ctx, struct vmod_priv *priv,
	VCL_STRING key, VCL_INT offset, VCL_INT initial, VCL_INT expiration)
{
	memcached_return_t rc;
	uint64_t value = 0;
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;
	memcached_st *mc = get_memcached(ctx, settings);

	if (!mc)
	{
		return settings->error_int;
	}

	memcached_increment_with_initial(mc, key, strlen(key), offset,
		initial, expiration, &value);

	rc = memcached_last_error(mc);

	release_memcached(ctx, settings, mc);

	if(rc)
	{
		return settings->error_int;
	}

	return (VCL_INT)value;
}

VCL_INT vmod_decr_set(const struct vrt_ctx *ctx, struct vmod_priv *priv,
	VCL_STRING key, VCL_INT offset, VCL_INT initial, VCL_INT expiration)
{
	memcached_return_t rc;
	uint64_t value = 0;
	vmod_mc_vcl_settings *settings = (vmod_mc_vcl_settings*)priv->priv;
	memcached_st *mc = get_memcached(ctx, settings);

	if (!mc)
	{
		return settings->error_int;
	}

	memcached_decrement_with_initial(mc, key, strlen(key), offset,
		initial, expiration, &value);

	rc = memcached_last_error(mc);

	release_memcached(ctx, settings, mc);

	if(rc)
	{
		return settings->error_int;
	}

	return (VCL_INT)value;
}
