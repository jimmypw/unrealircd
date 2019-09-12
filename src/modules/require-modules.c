/*
 * Check for modules that are required across the network, as well as modules
 * that *aren't* even allowed (deny/require module { } blocks)
 * (C) Copyright 2019 Gottem and the UnrealIRCd team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "unrealircd.h"

#define MSG_REQMODS "REQMODS"

ModuleHeader MOD_HEADER(require_modules) = {
	"require-modules",
	"5.0",
	"Check for required modules across the network",
	"UnrealIRCd Team",
	"unrealircd-5",
};

typedef struct _denymod DenyMod;
struct _denymod {
	DenyMod *prev, *next;
	char *name;
	char *reason;
};

// Forward declarations
Module *find_modptr_byname(char *name, unsigned strict);
DenyMod *find_denymod_byname(char *name);

int reqmods_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int reqmods_configrun(ConfigFile *cf, ConfigEntry *ce, int type);

int reqmods_configtest_deny(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int reqmods_configrun_deny(ConfigFile *cf, ConfigEntry *ce, int type);

int reqmods_configtest_require(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int reqmods_configrun_require(ConfigFile *cf, ConfigEntry *ce, int type);

int reqmods_configtest_set(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int reqmods_configrun_set(ConfigFile *cf, ConfigEntry *ce, int type);

CMD_FUNC(require_modules);
int reqmods_hook_serverconnect(Client *sptr);

// Globals
extern Module *Modules;
DenyMod *DenyModList = NULL;

struct cfgstruct {
	int squit_on_deny;
	int squit_on_missing;
	int squit_on_mismatch;
};
static struct cfgstruct cfg;

MOD_TEST(require_modules)
{
	memset(&cfg, 0, sizeof(cfg));
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, reqmods_configtest);
	return MOD_SUCCESS;
}

MOD_INIT(require_modules)
{
	MARK_AS_OFFICIAL_MODULE(modinfo);
	MARK_AS_GLOBAL_MODULE(modinfo);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, reqmods_configrun);
	HookAdd(modinfo->handle, HOOKTYPE_SERVER_CONNECT, 0, reqmods_hook_serverconnect);
	CommandAdd(modinfo->handle, MSG_REQMODS, require_modules, MAXPARA, M_SERVER);
	return MOD_SUCCESS;
}

MOD_LOAD(require_modules)
{
	if (ModuleGetError(modinfo->handle) != MODERR_NOERROR)
	{
		config_error("A critical error occurred when loading module %s: %s", MOD_HEADER(require_modules).name, ModuleGetErrorStr(modinfo->handle));
		return MOD_FAILED;
	}
	return MOD_SUCCESS;
}

MOD_UNLOAD(require_modules)
{
	DenyMod *dmod, *next;
	for (dmod = DenyModList; dmod; dmod = next)
	{
		next = dmod->next;
		MyFree(dmod->name);
		MyFree(dmod->reason);
		DelListItem(dmod, DenyModList);
		MyFree(dmod);
	}
	DenyModList = NULL;
	return MOD_SUCCESS;
}

Module *find_modptr_byname(char *name, unsigned strict)
{
	Module *mod;
	for (mod = Modules; mod; mod = mod->next)
	{
		// Let's not be too strict with the name
		if (!strcasecmp(mod->header->name, name))
		{
			if (strict && !(mod->flags & MODFLAG_LOADED))
				mod = NULL;
			return mod;
		}
	}
	return NULL;
}

DenyMod *find_denymod_byname(char *name)
{
	DenyMod *dmod;
	for (dmod = DenyModList; dmod; dmod = dmod->next)
	{
		if (!strcasecmp(dmod->name, name))
			return dmod;
	}
	return NULL;
}

int reqmods_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	if (type == CONFIG_DENY)
		return reqmods_configtest_deny(cf, ce, type, errs);

	if (type == CONFIG_REQUIRE)
		return reqmods_configtest_require(cf, ce, type, errs);

	if (type == CONFIG_SET)
		return reqmods_configtest_set(cf, ce, type, errs);

	return 0;
}

int reqmods_configrun(ConfigFile *cf, ConfigEntry *ce, int type)
{
	if (type == CONFIG_DENY)
		return reqmods_configrun_deny(cf, ce, type);

	if (type == CONFIG_REQUIRE)
		return reqmods_configrun_require(cf, ce, type);

	if (type == CONFIG_SET)
		return reqmods_configrun_set(cf, ce, type);

	return 0;
}

int reqmods_configtest_deny(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep;
	int has_name;

	// We are only interested in deny module { }
	if (strcmp(ce->ce_vardata, "module"))
		return 0;

	has_name = 0;
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strlen(cep->ce_varname))
		{
			config_error("%s:%i: blank directive for deny module { } block", cep->ce_fileptr->cf_filename, cep->ce_varlinenum);
			errors++;
			continue;
		}

		if (!cep->ce_vardata || !strlen(cep->ce_vardata))
		{
			config_error("%s:%i: blank %s without value for deny module { } block", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
			errors++;
			continue;
		}

		if (!strcmp(cep->ce_varname, "name"))
		{
			// We do a loose check here because a module might not be fully loaded yet
			if (find_modptr_byname(cep->ce_vardata, 0))
			{
				config_error("[require-modules] Module '%s' was specified as denied but we've actually loaded it ourselves", cep->ce_vardata);
				errors++;
			}
			has_name = 1;
			continue;
		}

		if (!strcmp(cep->ce_varname, "reason")) // Optional
			continue;

		config_error("%s:%i: unknown directive %s for deny module { } block", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
		errors++;
	}

	if (!has_name)
	{
		config_error("%s:%i: missing required 'name' directive for deny module { } block", ce->ce_fileptr->cf_filename, ce->ce_varlinenum);
		errors++;
	}

	*errs = errors;
	return errors ? -1 : 1;
}

int reqmods_configrun_deny(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;
	DenyMod *dmod;

	if (strcmp(ce->ce_vardata, "module"))
		return 0;

	dmod = MyMallocEx(sizeof(DenyMod));
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strcmp(cep->ce_varname, "name"))
		{
			safestrdup(dmod->name, cep->ce_vardata);
			continue;
		}

		if (!strcmp(cep->ce_varname, "reason"))
		{
			safestrdup(dmod->reason, cep->ce_vardata);
			continue;
		}
	}

	// Just use a somewhat cryptic default reason if none was specified (since it's optional)
	if (!dmod->reason || !strlen(dmod->reason))
		 safestrdup(dmod->reason, "A forbidden module is being used");
	AddListItem(dmod, DenyModList);
	return 1;
}

int reqmods_configtest_require(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep;
	int has_name;

	// We are only interested in require module { }
	if (strcmp(ce->ce_vardata, "module"))
		return 0;

	has_name = 0;
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strlen(cep->ce_varname))
		{
			config_error("%s:%i: blank directive for require module { } block", cep->ce_fileptr->cf_filename, cep->ce_varlinenum);
			errors++;
			continue;
		}

		if (!cep->ce_vardata || !strlen(cep->ce_vardata))
		{
			config_error("%s:%i: blank %s without value for require module { } block", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
			errors++;
			continue;
		}

		if (!strcmp(cep->ce_varname, "name"))
		{
			if (!find_modptr_byname(cep->ce_vardata, 0))
			{
				config_error("[require-modules] Module '%s' was specified as required but we didn't even load it ourselves (maybe double check the name?)", cep->ce_vardata);
				errors++;
			}

			// Let's be nice and let configrun handle the module flags
			has_name = 1;
			continue;
		}

		// Reason directive is not used for require module { }, so error on that too
		config_error("%s:%i: unknown directive %s for require module { } block", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
		errors++;
	}

	if (!has_name)
	{
		config_error("%s:%i: missing required 'name' directive for require module { } block", ce->ce_fileptr->cf_filename, ce->ce_varlinenum);
		errors++;
	}

	*errs = errors;
	return errors ? -1 : 1;
}

int reqmods_configrun_require(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;
	Module *mod;

	if (strcmp(ce->ce_vardata, "module"))
		return 0;

	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strcmp(cep->ce_varname, "name"))
		{
			if (!(mod = find_modptr_byname(cep->ce_vardata, 0)))
			{
				// Something went very wrong :D
				config_error("[require-modules] [BUG?] Passed configtest_require() but not configrun_require() for module '%s' (seems to not be loaded after all)", cep->ce_vardata);
				continue;
			}

			// Just add the global flag so we don't have to keep a separate list for required modules too =]
			if (!(mod->options & MOD_OPT_GLOBAL))
				mod->options |= MOD_OPT_GLOBAL;
			continue;
		}
	}

	return 1;
}

int reqmods_configtest_set(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep;

	// We are only interested in set::require-modules
	if (strcmp(ce->ce_varname, "require-modules"))
		return 0;

	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strlen(cep->ce_varname))
		{
			config_error("%s:%i: blank set::require-modules directive", cep->ce_fileptr->cf_filename, cep->ce_varlinenum);
			errors++;
			continue;
		}

		if (!cep->ce_vardata || !strlen(cep->ce_vardata))
		{
			config_error("%s:%i: blank set::require-modules::%s without value", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
			errors++;
			continue;
		}

		if (!strcmp(cep->ce_varname, "squit-on-deny") || !strcmp(cep->ce_varname, "squit-on-missing") || !strcmp(cep->ce_varname, "squit-on-mismatch"))
			continue;

		config_error("%s:%i: unknown directive set::require-modules::%s", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
		errors++;
	}

	*errs = errors;
	return errors ? -1 : 1;
}

int reqmods_configrun_set(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;

	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strcmp(cep->ce_varname, "squit-on-deny"))
		{
			cfg.squit_on_deny = config_checkval(cep->ce_vardata, CFG_YESNO);
			continue;
		}

		if (!strcmp(cep->ce_varname, "squit-on-missing"))
		{
			cfg.squit_on_missing = config_checkval(cep->ce_vardata, CFG_YESNO);
			continue;
		}

		if (!strcmp(cep->ce_varname, "squit-on-mismatch"))
		{
			cfg.squit_on_mismatch = config_checkval(cep->ce_vardata, CFG_YESNO);
			continue;
		}
	}

	return 1;
}

CMD_FUNC(require_modules)
{
	char flag, name[64], *version;
	char buf[BUFSIZE];
	char *p, *modbuf;
	Module *mod;
	DenyMod *dmod;
	int i;

	// A non-server sptr shouldn't really be possible here, but still :D
	if (!MyConnect(sptr) || !IsServer(sptr) || BadPtr(parv[1]))
		return 0;

	// Module strings are passed as 1 parameter
	strlcpy(buf, parv[1], sizeof(buf));
	for (modbuf = strtoken(&p, buf, " "); modbuf; modbuf = strtoken(&p, NULL, " "))
	{
		flag = *modbuf++; // Get the local/global flag
		strlcpy(name, modbuf, sizeof(name)); // Let's work on a copy of the param
		if ((version = strstr(name, ":")))
			*version++ = '\0';

		// Even if a denied module is only required locally, maybe still prevent a server that uses it from linking in
		if ((dmod = find_denymod_byname(name)))
		{
			// Send this particular notice to local opers only
			sendto_umode(UMODE_OPER, "Server %s is using module '%s' which is specified in a deny module { } config block (reason: %s)", sptr->name, name, dmod->reason);
			if (cfg.squit_on_deny) // If set to SQUIT, simply use the reason as-is
			{
				sendto_umode_global(UMODE_OPER, "ABORTING LINK: %s <=> %s (reason: %s)", me.name, sptr->name, dmod->reason);
				return exit_client(cptr, sptr, &me, NULL, dmod->reason);
			}
			continue;
		}

		// Doing a strict check for the module being fully loaded so we can emit a warning in that case too :>
		if (!(mod = find_modptr_byname(name, 1)))
		{
			/* Since only the server missing the module will report it, we need to broadcast the warning network-wide ;]
			 * Obviously we won't send this notice if the module seems to be locally required only
			 */
			if (flag == 'G')
			{
				sendto_umode_global(UMODE_OPER, "Globally required module '%s' wasn't (fully) loaded or is missing entirely", name);
				if (cfg.squit_on_missing)
				{
					sendto_umode_global(UMODE_OPER, "ABORTING LINK: %s <=> %s", me.name, sptr->name);
					return exit_client(cptr, sptr, &me, NULL, "Missing globally required module");
				}
			}
			continue;
		}

		/* A strcasecmp() suffices because the version string only has to *start* with a digit, it can have e.g. "-alpha" at the end
		 * Also, if the version bit is dropped of for some weird reason, we'll treat it as a mismatch too
		 * Furthermore, we check the module version for locally required modules as well (for completeness)
		 */
		if (!version || strcasecmp(mod->header->version, version))
		{
			// Version mismatches can be (and are) reported on both ends separately, so a local server notice is enough
			sendto_umode(UMODE_OPER, "Version mismatch for module '%s' (ours: %s, theirs: %s)", name, mod->header->version, version);
			if (cfg.squit_on_mismatch)
			{
				sendto_umode_global(UMODE_OPER, "ABORTING LINK: %s <=> %s", me.name, sptr->name);
				return exit_client(cptr, sptr, &me, NULL, "Module version mismatch");
			}
			continue;
		}
	}

	return 0;
}

int reqmods_hook_serverconnect(Client *sptr)
{
	/* This function simply dumps a list of modules and their version to the other server,
	 * which will then run through the received list and check the names/versions
	 */
	char modbuf[64];
	char sendbuf[BUFSIZE - HOSTLEN - 4]; // Try to use a large as buffer as possible (while accounting for ":<server name> ")
	Module *mod;
	size_t len, modlen;
	size_t bufsize, modsize;
	int count;

	/* Let's not have leaves directly connected to the hub send their module list to other *leaves* as well =]
	 * Since the hub will introduce all servers currently linked to it, this POST_SERVER_CONNECT hook is
	 * actually called for every separate node
	 */
	if (!MyConnect(sptr))
		return HOOK_CONTINUE;

	sendbuf[0] = '\0';
	len = 0;
	count = 0;
	bufsize = sizeof(sendbuf);
	modsize = sizeof(modbuf);

	for (mod = Modules; mod; mod = mod->next)
	{
		/* At this stage we don't care if the module isn't global (or not fully loaded), we'll dump all modules
		 * so we can properly deny certain ones across the network
		 */
		ircsnprintf(modbuf, modsize, "%c%s:%s", ((mod->options & MOD_OPT_GLOBAL) ? 'G' : 'L'), mod->header->name, mod->header->version);
		modlen = strlen(modbuf);
		if ((len + modlen + 2) > bufsize) // Adding 2 to because 1) null byte 2) space between modules
		{
			// "Flush" current list =]
			sendto_one(sptr, NULL, ":%s %s :%s", me.id, MSG_REQMODS, sendbuf);
			sendbuf[0] = '\0';
			len = 0;
			count = 0;
		}

		ircsnprintf(sendbuf + len, bufsize - len, "%s%s", (len > 0 ? " " : ""), modbuf);
		if (len)
			len++; // Include the space if necessary
		len += modlen;
		count++;
	}

	// May have something left
	if (sendbuf[0])
		sendto_one(sptr, NULL, ":%s %s :%s", me.id, MSG_REQMODS, sendbuf);
	return HOOK_CONTINUE;
}