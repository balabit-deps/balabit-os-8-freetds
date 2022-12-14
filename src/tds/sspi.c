/* FreeTDS - Library of routines accessing Sybase and Microsoft databases
 * Copyright (C) 2010  Frediano Ziglio
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <config.h>

/* enabled some additional definitions for getaddrinfo */
#define _WIN32_WINNT 0x601

/* fix possible bug in sspi.h header */
#define FreeCredentialHandle FreeCredentialsHandle

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif /* HAVE_STDLIB_H */

#if HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */

#if HAVE_SSPI
#define SECURITY_WIN32

#include <freetds/windows.h>
#include <security.h>
#include <sspi.h>
#include <rpc.h>

#include <freetds/tds.h>
#include <freetds/thread.h>
#include <freetds/utils/string.h>
#include <freetds/bool.h>
#include "replacements.h"

/**
 * \ingroup libtds
 * \defgroup auth Authentication
 * Functions for handling authentication.
 */

/**
 * \addtogroup auth
 * @{
 */

typedef struct tds_sspi_auth
{
	TDSAUTHENTICATION tds_auth;
	CredHandle cred;
	CtxtHandle cred_ctx;
	char *sname;
} TDSSSPIAUTH;

static HMODULE secdll = NULL;
static PSecurityFunctionTableA sec_fn = NULL;
static tds_mutex sec_mutex = TDS_MUTEX_INITIALIZER;

static bool
tds_init_secdll(void)
{
	bool res = false;

	if (sec_fn)
		return true;

	tds_mutex_lock(&sec_mutex);
	for (;;) {
		if (!secdll) {
			secdll = LoadLibrary(TEXT("secur32.dll"));
			if (!secdll)
				break;
		}
		if (!sec_fn) {
			INIT_SECURITY_INTERFACE_A pInitSecurityInterface;

			pInitSecurityInterface = (INIT_SECURITY_INTERFACE_A) GetProcAddress(secdll, "InitSecurityInterfaceA");
			if (!pInitSecurityInterface)
				break;

			sec_fn = pInitSecurityInterface();
			if (!sec_fn)
				break;
		}
		res = true;
		break;
	}
	tds_mutex_unlock(&sec_mutex);
	return res;
}

static int
tds_sspi_free(TDSCONNECTION * conn, struct tds_authentication * tds_auth)
{
	TDSSSPIAUTH *auth = (TDSSSPIAUTH *) tds_auth;

	if (SecIsValidHandle(&auth->cred_ctx))
		sec_fn->DeleteSecurityContext(&auth->cred_ctx);
	if (SecIsValidHandle(&auth->cred))
		sec_fn->FreeCredentialsHandle(&auth->cred);
	if (auth->tds_auth.packet)
		sec_fn->FreeContextBuffer(auth->tds_auth.packet);
	free(auth->sname);
	free(auth);
	return TDS_SUCCESS;
}

static int
tds_sspi_handle_next(TDSSOCKET * tds, struct tds_authentication * tds_auth, size_t len)
{
	SecBuffer in_buf, out_buf;
	SecBufferDesc in_desc, out_desc;
	SECURITY_STATUS status;
	ULONG attrs;
	TimeStamp ts;
	uint8_t *auth_buf;

	TDSSSPIAUTH *auth = (TDSSSPIAUTH *) tds_auth;

	if (len < 1)
		return TDS_FAIL;

	auth_buf = tds_new(uint8_t, len);
	if (!auth_buf)
		return TDS_FAIL;
	tds_get_n(tds, auth_buf, (int)len);

	/* free previously allocated buffer */
	if (auth->tds_auth.packet) {
		sec_fn->FreeContextBuffer(auth->tds_auth.packet);
		auth->tds_auth.packet = NULL;
	}
	in_desc.ulVersion  = out_desc.ulVersion  = SECBUFFER_VERSION;
	in_desc.cBuffers   = out_desc.cBuffers   = 1;
	in_desc.pBuffers   = &in_buf;
	out_desc.pBuffers   = &out_buf;

	in_buf.BufferType = SECBUFFER_TOKEN;
	in_buf.pvBuffer   = auth_buf;
	in_buf.cbBuffer   = (ULONG)len;

	out_buf.BufferType = SECBUFFER_TOKEN;
	out_buf.pvBuffer   = NULL;
	out_buf.cbBuffer   = 0;

	status = sec_fn->InitializeSecurityContext(&auth->cred, &auth->cred_ctx, auth->sname,
		ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONNECTION | ISC_REQ_ALLOCATE_MEMORY,
		0, SECURITY_NETWORK_DREP, &in_desc,
		0, &auth->cred_ctx, &out_desc,
		&attrs, &ts);

	free(auth_buf);
	auth->tds_auth.packet = out_buf.pvBuffer;

	switch (status) {
	case SEC_I_COMPLETE_AND_CONTINUE:
		sec_fn->CompleteAuthToken(&auth->cred_ctx, &out_desc);
		break;

	case SEC_I_CONTINUE_NEEDED:
	case SEC_E_OK:
		break;

	default:
		return TDS_FAIL;
	}

	if (out_buf.cbBuffer == 0)
		return TDS_SUCCESS;

	tds_put_n(tds, auth->tds_auth.packet, out_buf.cbBuffer);

	return tds_flush_packet(tds);
}

/**
 * Build a SSPI packet to send to server
 * @param tds     A pointer to the TDSSOCKET structure managing a client/server operation.
 */
TDSAUTHENTICATION *
tds_sspi_get_auth(TDSSOCKET * tds)
{
	SecBuffer buf;
	SecBufferDesc desc;
	SECURITY_STATUS status;
	ULONG attrs;
	TimeStamp ts;
	SEC_WINNT_AUTH_IDENTITY identity;
	const char *p, *user_name, *server_name;
	struct addrinfo *addrs = NULL;

	TDSSSPIAUTH *auth;
	TDSLOGIN *login = tds->login;

	/* check login */
	if (!login)
		return NULL;

	if (!tds_init_secdll())
		return NULL;

	/* parse username/password information */
	memset(&identity, 0, sizeof(identity));
	user_name = tds_dstr_cstr(&login->user_name);
	if ((p = strchr(user_name, '\\')) != NULL) {
		identity.Flags = SEC_WINNT_AUTH_IDENTITY_ANSI;
		identity.Password = (void *) tds_dstr_cstr(&login->password);
		identity.PasswordLength = tds_dstr_len(&login->password);
		identity.Domain = (void *) user_name;
		identity.DomainLength = p - user_name;
		user_name = p + 1;
		identity.User = (void *) user_name;
		identity.UserLength = strlen(user_name);
	}

	auth = tds_new0(TDSSSPIAUTH, 1);
	if (!auth)
		return NULL;

	auth->tds_auth.free = tds_sspi_free;
	auth->tds_auth.handle_next = tds_sspi_handle_next;
	SecInvalidateHandle(&auth->cred);
	SecInvalidateHandle(&auth->cred_ctx);

	/* using Negotiate system will use proper protocol (either NTLM or Kerberos) */
	if (sec_fn->AcquireCredentialsHandle(NULL, (char *)"Negotiate", SECPKG_CRED_OUTBOUND,
		NULL, identity.Domain ? &identity : NULL,
		NULL, NULL, &auth->cred, &ts) != SEC_E_OK) {
		free(auth);
		return NULL;
	}

	desc.ulVersion = SECBUFFER_VERSION;
	desc.cBuffers  = 1;
	desc.pBuffers  = &buf;

	buf.BufferType = SECBUFFER_TOKEN;
	buf.pvBuffer   = NULL;
	buf.cbBuffer   = 0;

	/* build SPN */
	server_name = tds_dstr_cstr(&login->server_host_name);
	if (strchr(server_name, '.') == NULL) {
		struct addrinfo hints;
		int res;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_V4MAPPED|AI_ADDRCONFIG|AI_CANONNAME|AI_FQDN;
		res = getaddrinfo(server_name, NULL, &hints, &addrs);
		if (res) {
			/* some version of Windows does not support V4MAPPED
			 * and ADDRCONFIG, try without them
			 */
			hints.ai_flags = AI_CANONNAME|AI_FQDN;
			res = getaddrinfo(server_name, NULL, &hints, &addrs);
		}
		if (!res && addrs->ai_canonname && strchr(addrs->ai_canonname, '.') != NULL)
			server_name = addrs->ai_canonname;
	}
	if (strchr(server_name, '.') != NULL) {
		if (asprintf(&auth->sname, "MSSQLSvc/%s:%d", server_name, login->port) < 0) {
			if (addrs)
				freeaddrinfo(addrs);
			tds_sspi_free(tds->conn, &auth->tds_auth);
			return NULL;
		}
		tdsdump_log(TDS_DBG_NETWORK, "kerberos name %s\n", auth->sname);
	}
	if (addrs)
		freeaddrinfo(addrs);

	status = sec_fn->InitializeSecurityContext(&auth->cred, NULL, auth->sname,
		ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONNECTION | ISC_REQ_ALLOCATE_MEMORY,
		0, SECURITY_NETWORK_DREP,
		NULL, 0,
		&auth->cred_ctx, &desc,
		&attrs, &ts);

	switch (status) {
	case SEC_I_COMPLETE_AND_CONTINUE:
		sec_fn->CompleteAuthToken(&auth->cred_ctx, &desc);
		break;

	case SEC_I_CONTINUE_NEEDED:
	case SEC_E_OK:
		break;

	default:
		tds_sspi_free(tds->conn, &auth->tds_auth);
		return NULL;
	}

	auth->tds_auth.packet_len = buf.cbBuffer;
	auth->tds_auth.packet     = buf.pvBuffer;

	return &auth->tds_auth;
}

#endif /* HAVE_SSPI */

/** @} */

