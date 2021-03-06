/*
 * rlm_eap_peap.c  contains the interfaces that are called from eap
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2003 Alan DeKok <aland@freeradius.org>
 * Copyright 2006 The FreeRADIUS server project
 */
RCSID("$Id$")

#define LOG_PREFIX "rlm_eap_peap - "

#include "eap_peap.h"

typedef struct rlm_eap_peap_t {
	char const		*tls_conf_name;		//!< TLS configuration.
	fr_tls_conf_t	*tls_conf;

	char const		*inner_eap_module;	//!< module name for inner EAP
	int			auth_type_eap;
	bool			use_tunneled_reply;	//!< Use the reply attributes from the tunneled session in
							//!< the non-tunneled reply to the client.

	bool			copy_request_to_tunnel;	//!< Use SOME of the request attributes from outside of the
							//!< tunneled session in the tunneled request.
#ifdef WITH_PROXY
	bool			proxy_tunneled_request_as_eap;	//!< Proxy tunneled session as EAP, or as de-capsulated
							//!< protocol.
#endif
	char const		*virtual_server;	//!< Virtual server for inner tunnel session.

	bool			soh;			//!< Do we do SoH request?
	char const		*soh_virtual_server;
	bool			req_client_cert;	//!< Do we do require a client cert?
} rlm_eap_peap_t;


static CONF_PARSER submodule_config[] = {
	{ FR_CONF_OFFSET("tls", PW_TYPE_STRING, rlm_eap_peap_t, tls_conf_name) },

	{ FR_CONF_OFFSET("inner_eap_module", PW_TYPE_STRING, rlm_eap_peap_t, inner_eap_module), },

	{ FR_CONF_DEPRECATED("copy_request_to_tunnel", PW_TYPE_BOOLEAN, rlm_eap_peap_t, NULL), .dflt = "no" },

	{ FR_CONF_DEPRECATED("use_tunneled_reply", PW_TYPE_BOOLEAN, rlm_eap_peap_t, NULL), .dflt = "no" },

#ifdef WITH_PROXY
	{ FR_CONF_OFFSET("proxy_tunneled_request_as_eap", PW_TYPE_BOOLEAN, rlm_eap_peap_t, proxy_tunneled_request_as_eap), .dflt = "yes" },
#endif

	{ FR_CONF_OFFSET("virtual_server", PW_TYPE_STRING | PW_TYPE_REQUIRED | PW_TYPE_NOT_EMPTY, rlm_eap_peap_t, virtual_server) },

	{ FR_CONF_OFFSET("soh", PW_TYPE_BOOLEAN, rlm_eap_peap_t, soh), .dflt = "no" },

	{ FR_CONF_OFFSET("require_client_cert", PW_TYPE_BOOLEAN, rlm_eap_peap_t, req_client_cert), .dflt = "no" },

	{ FR_CONF_OFFSET("soh_virtual_server", PW_TYPE_STRING, rlm_eap_peap_t, soh_virtual_server) },
	CONF_PARSER_TERMINATOR
};

/*
 *	Allocate the PEAP per-session data
 */
static peap_tunnel_t *peap_alloc(TALLOC_CTX *ctx, rlm_eap_peap_t *inst)
{
	peap_tunnel_t *t;

	t = talloc_zero(ctx, peap_tunnel_t);

#ifdef WITH_PROXY
	t->proxy_tunneled_request_as_eap = inst->proxy_tunneled_request_as_eap;
#endif
	t->virtual_server = inst->virtual_server;
	t->soh = inst->soh;
	t->soh_virtual_server = inst->soh_virtual_server;
	t->session_resumption_state = PEAP_RESUMPTION_MAYBE;

	return t;
}

/*
 *	Do authentication, by letting EAP-TLS do most of the work.
 */
static rlm_rcode_t CC_HINT(nonnull) mod_process(void *instance, eap_session_t *eap_session);
static rlm_rcode_t mod_process(void *arg, eap_session_t *eap_session)
{
	int			rcode;
	eap_tls_status_t	status;
	rlm_eap_peap_t		*inst = (rlm_eap_peap_t *) arg;

	eap_tls_session_t	*eap_tls_session = talloc_get_type_abort(eap_session->opaque, eap_tls_session_t);
	tls_session_t		*tls_session = eap_tls_session->tls_session;
	peap_tunnel_t		*peap = NULL;
	REQUEST			*request = eap_session->request;

	if (tls_session->opaque) {
		peap = talloc_get_type_abort(tls_session->opaque, peap_tunnel_t);
	/*
	 *	Session resumption requires the storage of data, so
	 *	allocate it if it doesn't already exist.
	 */
	} else {
		peap = tls_session->opaque = peap_alloc(tls_session, inst);
	}

	status = eap_tls_process(eap_session);
	if ((status == EAP_TLS_INVALID) || (status == EAP_TLS_FAIL)) {
		REDEBUG("[eap-tls process] = %s", fr_int2str(eap_tls_status_table, status, "<INVALID>"));
	} else {
		RDEBUG2("[eap-tls process] = %s", fr_int2str(eap_tls_status_table, status, "<INVALID>"));
	}

	switch (status) {
	/*
	 *	EAP-TLS handshake was successful, tell the
	 *	client to keep talking.
	 *
	 *	If this was EAP-TLS, we would just return
	 *	an EAP-TLS-Success packet here.
	 */
	case EAP_TLS_ESTABLISHED:
		peap->status = PEAP_STATUS_TUNNEL_ESTABLISHED;
		break;

	/*
	 *	The TLS code is still working on the TLS
	 *	exchange, and it's a valid TLS request.
	 *	do nothing.
	 */
	case EAP_TLS_HANDLED:
		/*
		 *	FIXME: If the SSL session is established, grab the state
		 *	and EAP id from the inner tunnel, and update it with
		 *	the expected EAP id!
		 */
		return 1;

	/*
	 *	Handshake is done, proceed with decoding tunneled
	 *	data.
	 */
	case EAP_TLS_RECORD_RECV_COMPLETE:
		break;

	/*
	 *	Anything else: fail.
	 */
	default:
		return 0;
	}

	/*
	 *	Session is established, proceed with decoding
	 *	tunneled data.
	 */
	RDEBUG2("Session established.  Decoding tunneled data");

	/*
	 *	We may need PEAP data associated with the session, so
	 *	allocate it here, if it wasn't already alloacted.
	 */
	if (!tls_session->opaque) tls_session->opaque = peap_alloc(tls_session, inst);

	/*
	 *	Process the PEAP portion of the request.
	 */
	rcode = eap_peap_process(eap_session, tls_session, inst->auth_type_eap);
	switch (rcode) {
	case RLM_MODULE_REJECT:
		eap_tls_fail(eap_session);
		return 0;

	case RLM_MODULE_HANDLED:
		eap_tls_request(eap_session);
		return 1;

	case RLM_MODULE_OK:
		/*
		 *	Success: Automatically return MPPE keys.
		 */
		if (eap_tls_success(eap_session) < 0) return 0;
		return 1;

		/*
		 *	No response packet, MUST be proxying it.
		 *	The main EAP module will take care of discovering
		 *	that the request now has a "proxy" packet, and
		 *	will proxy it, rather than returning an EAP packet.
		 */
	case RLM_MODULE_UPDATED:
#ifdef WITH_PROXY
		rad_assert(eap_session->request->proxy != NULL);
#endif
		return 1;

	default:
		break;
	}

	eap_tls_fail(eap_session);
	return 0;
}

/*
 *	Send an initial eap-tls request to the peer, using the libeap functions.
 */
static rlm_rcode_t mod_session_init(void *type_arg, eap_session_t *eap_session)
{
	eap_tls_session_t	*eap_tls_session;
	rlm_eap_peap_t		*inst = talloc_get_type_abort(type_arg, rlm_eap_peap_t);
	VALUE_PAIR		*vp;
	bool			client_cert;

	eap_session->tls = true;

	/*
	 *	EAP-TLS-Require-Client-Cert attribute will override
	 *	the require_client_cert configuration option.
	 */
	vp = fr_pair_find_by_num(eap_session->request->control, 0, PW_EAP_TLS_REQUIRE_CLIENT_CERT, TAG_ANY);
	if (vp) {
		client_cert = vp->vp_integer ? true : false;
	} else {
		client_cert = inst->req_client_cert;
	}

	eap_session->opaque = eap_tls_session = eap_tls_session_init(eap_session, inst->tls_conf, client_cert);
	if (!eap_tls_session) return 0;

	/*
	 *	Set up type-specific information.
	 */
	eap_tls_session->tls_session->prf_label = "client EAP encryption";

	/*
	 *	As it is a poorly designed protocol, PEAP uses
	 *	bits in the TLS header to indicate PEAP
	 *	version numbers.  For now, we only support
	 *	PEAP version 0, so it doesn't matter too much.
	 *	However, if we support later versions of PEAP,
	 *	we will need this flag to indicate which
	 *	version we're currently dealing with.
	 */
	eap_tls_session->base_flags = 0x00;

	/*
	 *	PEAP version 0 requires 'include_length = no',
	 *	so rather than hoping the user figures it out,
	 *	we force it here.
	 */
	eap_tls_session->include_length = false;

	/*
	 *	TLS session initialization is over.  Now handle TLS
	 *	related handshaking or application data.
	 */
	if (eap_tls_start(eap_session) < 0) {
		talloc_free(eap_tls_session);
		return 0;
	}

	eap_session->process = mod_process;

	return 1;
}

/*
 *	Attach the module.
 */
static int mod_instantiate(UNUSED rlm_eap_config_t const *config, void *instance, CONF_SECTION *cs)
{
	rlm_eap_peap_t		*inst = talloc_get_type_abort(instance, rlm_eap_peap_t);
	fr_dict_enum_t		*dv;

	if (!cf_section_sub_find_name2(main_config.config, "server", inst->virtual_server)) {
		cf_log_err_by_name(cs, "virtual_server", "Unknown virtual server '%s'", inst->virtual_server);
		return -1;
	}

	if (inst->soh_virtual_server) {
		if (!cf_section_sub_find_name2(main_config.config, "server", inst->soh_virtual_server)) {
			cf_log_err_by_name(cs, "soh_virtual_server", "Unknown virtual server '%s'", inst->virtual_server);
			return -1;
		}
	}

	/*
	 *	Read tls configuration, either from group given by 'tls'
	 *	option, or from the eap-tls configuration.
	 */
	inst->tls_conf = eap_tls_conf_parse(cs, "tls");
	if (!inst->tls_conf) {
		ERROR("Failed initializing SSL context");
		return -1;
	}

	/*
	 *	Don't expose this if we don't need it.
	 */
	if (!inst->inner_eap_module) inst->inner_eap_module = "eap";

	dv = fr_dict_enum_by_name(NULL, fr_dict_attr_by_num(NULL, 0, PW_AUTH_TYPE), inst->inner_eap_module);
	if (!dv) {
		WARN("Failed to find 'Auth-Type %s' section in virtual server %s.  "
		     "The server cannot proxy inner-tunnel EAP packets",
		     inst->inner_eap_module, inst->virtual_server);
	} else {
		inst->auth_type_eap = dv->value;
	}

	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 */
extern rlm_eap_submodule_t rlm_eap_peap;
rlm_eap_submodule_t rlm_eap_peap = {
	.name		= "eap_peap",
	.magic		= RLM_MODULE_INIT,

	.inst_size	= sizeof(rlm_eap_peap_t),
	.config		= submodule_config,
	.instantiate	= mod_instantiate,

	.session_init	= mod_session_init,	/* Initialise a new EAP session */
	.process	= mod_process		/* Process next round of EAP method */
};
