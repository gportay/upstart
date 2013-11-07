/* upstart
 *
 * Copyright © 2013 Canonical Ltd.
 * Author: Gaël Portay <gael.portay@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <ctype.h>

#include <net/if.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/list.h>
#include <nih/io.h>
#include <nih/option.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <nih/error.h>

#include <nih-dbus/errors.h>
#include <nih-dbus/dbus_error.h>
#include <nih-dbus/dbus_connection.h>
#include <nih-dbus/dbus_proxy.h>

#include "dbus/upstart.h"
#include "com.ubuntu.Upstart.h"


/* Prototypes for static functions */
static void upstart_disconnected (DBusConnection *connection);
static void upstart_forward_event (void           *data,
				   NihDBusMessage *message,
				   const char     *path);
static void system_disconnected (DBusConnection *connection);
static void emit_event_error (void *data, NihDBusMessage *message);
static NihDBusProxy * avahi_create_browser_proxy (const char *type,
						  const char *domain);
static NihDBusProxy * service_type_browser_new (NihDBusProxy *proxy,
						int32_t       interface,
						int32_t       protocol,
						const char   *domain,
						uint32_t      flags);
static int service_type_browser_free (NihDBusProxy *proxy);
static NihDBusProxy *service_browser_new (NihDBusProxy *proxy,
					  int32_t       interface,
					  int32_t       protocol,
					  const char   *type,
					  const char   *domain,
					  uint32_t      flags);
static int service_browser_free (NihDBusProxy *proxy);
static int emit_event (const char *action,
		       int32_t     interface,
		       int32_t     protocol,
		       const char *name,
		       const char *type,
		       const char *domain,
		       uint32_t    flags);

/**
 * daemonise:
 *
 * Set to TRUE if we should become a daemon, rather than just running
 * in the foreground.
 **/
static int daemonise = FALSE;

/**
 * upstart:
 *
 * Proxy to Upstart daemon.
 **/
static NihDBusProxy *upstart = NULL;

/**
 * avahi:
 *
 * Proxy to system Avahi daemon.
 **/
static NihDBusProxy *avahi = NULL;

/**
 * avahi_type_browser:
 *
 * Proxy to system Avahi Service Browser Type.
 **/
static NihDBusProxy *avahi_type_browser = NULL;

/**
 * type_entries:
 *
 * List of type entry.
 */
NihList type_entries;

/**
 * browser_entries:
 *
 * List of type entry.
 */
NihList browser_entries;

/**
 * options:
 *
 * Command-line options accepted by this program.
 **/
static NihOption options[] = {
	{ 0, "daemon", N_("Detach and run in the background"),
	  NULL, NULL, &daemonise, NULL },
	NIH_OPTION_LAST
};


/**
 * org_freedesktop_Avahi_Server_Type_Browser_ItemNew_signal_args:
 *
 * Arguments list for ItemNew signal.
 */
static const NihDBusArg org_freedesktop_Avahi_Server_Type_Browser_ItemNew_signal_args[] = {
	{ "interface", "i", NIH_DBUS_ARG_OUT },
	{ "protocol",  "i", NIH_DBUS_ARG_OUT },
	{ "type",      "s", NIH_DBUS_ARG_OUT },
	{ "domain",    "s", NIH_DBUS_ARG_OUT },
	{ "flags",     "u", NIH_DBUS_ARG_OUT },
	{ NULL }
};

/**
 * org_freedesktop_Avahi_Server_Type_Browser_ItemRemove_signal_args:
 *
 * Arguments list for ItemRemove signal.
 */
static const NihDBusArg org_freedesktop_Avahi_Server_Type_Browser_ItemRemove_signal_args[] = {
	{ "interface", "i", NIH_DBUS_ARG_OUT },
	{ "protocol",  "i", NIH_DBUS_ARG_OUT },
	{ "type",      "s", NIH_DBUS_ARG_OUT },
	{ "domain",    "s", NIH_DBUS_ARG_OUT },
	{ "flags",     "u", NIH_DBUS_ARG_OUT },
	{ NULL }
};

/**
 * org_freedesktop_Avahi_Server_Type_Browser_AllForNow_signal_args:
 *
 * Arguments list for AllForNow signal.
 */
static const NihDBusArg org_freedesktop_Avahi_Server_Type_Browser_AllForNow_signal_args[] = {
	{ NULL }
};

static DBusHandlerResult
org_freedesktop_Avahi_Server_Type_Browser_ItemNew_signal (
		DBusConnection     *connection,
		DBusMessage        *signal,
		NihDBusProxySignal *proxied)
{
	NihDBusMessage *message;
	DBusError       error;
	int32_t         interface;
	int32_t         protocol;
	char           *type;
	char           *domain;
	uint32_t        flags;
	NihListEntry   *entry;

	nih_assert (connection != NULL);
	nih_assert (signal != NULL);
	nih_assert (proxied != NULL);
	nih_assert (connection == proxied->proxy->connection);

	if (! dbus_message_is_signal (signal, proxied->interface->name, proxied->signal->name))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (! dbus_message_has_path (signal, proxied->proxy->path))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (proxied->proxy->name)
		if (! dbus_message_has_sender (signal, proxied->proxy->owner))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	message = nih_dbus_message_new (NULL, connection, signal);
	if (! message)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Iterate the arguments to the signal and demarshal into arguments
	 * for our own function call.
	 */
	dbus_error_init (&error);

	if (! dbus_message_get_args(message->message, &error,
				    DBUS_TYPE_INT32,  &interface,
				    DBUS_TYPE_INT32,  &protocol,
				    DBUS_TYPE_STRING, &type,
				    DBUS_TYPE_STRING, &domain,
				    DBUS_TYPE_UINT32, &flags,
				    DBUS_TYPE_INVALID)) {
		dbus_error_free (&error);
		nih_free (message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_free (&error);

	entry = NIH_MUST (nih_list_entry_new (NULL));
	entry->data = nih_strdup (NULL, type);
	nih_list_add (&type_entries, &entry->entry);

	nih_free (message);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
org_freedesktop_Avahi_Server_Type_Browser_ItemRemove_signal (
		DBusConnection     *connection,
		DBusMessage        *signal,
		NihDBusProxySignal *proxied)
{
	NihDBusMessage *message;
	DBusError       error;
	int32_t         interface;
	int32_t         protocol;
	char           *type;
	char           *domain;
	uint32_t        flags;

	nih_assert (connection != NULL);
	nih_assert (signal != NULL);
	nih_assert (proxied != NULL);
	nih_assert (connection == proxied->proxy->connection);

	if (! dbus_message_is_signal (signal, proxied->interface->name, proxied->signal->name))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (! dbus_message_has_path (signal, proxied->proxy->path))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (proxied->proxy->name)
		if (! dbus_message_has_sender (signal, proxied->proxy->owner))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	message = nih_dbus_message_new (NULL, connection, signal);
	if (! message)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Iterate the arguments to the signal and demarshal into arguments
	 * for our own function call.
	 */
	dbus_error_init (&error);

	if (! dbus_message_get_args(message->message, &error,
				    DBUS_TYPE_INT32,  &interface,
				    DBUS_TYPE_INT32,  &protocol,
				    DBUS_TYPE_STRING, &type,
				    DBUS_TYPE_STRING, &domain,
				    DBUS_TYPE_UINT32, &flags,
				    DBUS_TYPE_INVALID)) {
		dbus_error_free (&error);
		nih_free (message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_free (&error);

	nih_free (message);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
org_freedesktop_Avahi_Server_Type_Browser_AllForNow_signal (
		DBusConnection     *connection,
		DBusMessage        *signal,
		NihDBusProxySignal *proxied)
{
	NihDBusMessage *message;
	DBusError       error;

	nih_assert (connection != NULL);
	nih_assert (signal != NULL);
	nih_assert (proxied != NULL);
	nih_assert (connection == proxied->proxy->connection);

	if (! dbus_message_is_signal (signal, proxied->interface->name, proxied->signal->name))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (! dbus_message_has_path (signal, proxied->proxy->path))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (proxied->proxy->name)
		if (! dbus_message_has_sender (signal, proxied->proxy->owner))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	message = nih_dbus_message_new (NULL, connection, signal);
	if (! message)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Iterate the arguments to the signal and demarshal into arguments
	 * for our own function call.
	 */
	dbus_error_init (&error);

	if (! dbus_message_get_args(message->message, &error,
				    DBUS_TYPE_INVALID)) {
		dbus_error_is_set (&error);
		dbus_error_free (&error);
		nih_free (message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_free (&error);

	NIH_LIST_FOREACH_SAFE (&type_entries, iter) {
		NihListEntry *type_entry = (NihListEntry *)iter;
		avahi_create_browser_proxy((const char *)type_entry->data, "");
		nih_free (type_entry->data);
		nih_list_remove (&type_entry->entry);
		nih_free (type_entry);
	}

	nih_free (message);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// org.freedesktop.Avahi.ServiceTypeBrowser
static const NihDBusSignal org_freedesktop_Avahi_ServiceTypeBrowser_signals[] = {
	{ "ItemNew",        org_freedesktop_Avahi_Server_Type_Browser_ItemNew_signal_args,        org_freedesktop_Avahi_Server_Type_Browser_ItemNew_signal        },
	{ "ItemRemove",     org_freedesktop_Avahi_Server_Type_Browser_ItemRemove_signal_args,     org_freedesktop_Avahi_Server_Type_Browser_ItemRemove_signal     },
	{ "AllForNow",      org_freedesktop_Avahi_Server_Type_Browser_AllForNow_signal_args,      org_freedesktop_Avahi_Server_Type_Browser_AllForNow_signal      },
	{ NULL }
};

static const NihDBusProperty org_freedesktop_Avahi_ServiceTypeBrowser_properties[] = {
	{ "version",      "s", NIH_DBUS_READ,      NULL, NULL },
	{ "log_priority", "s", NIH_DBUS_READWRITE, NULL, NULL },
	{ NULL }
};

const NihDBusInterface org_freedesktop_Avahi_ServiceTypeBrowser = {
	"org.freedesktop.Avahi.ServiceTypeBrowser",
	NULL,
	org_freedesktop_Avahi_ServiceTypeBrowser_signals,
	org_freedesktop_Avahi_ServiceTypeBrowser_properties
};

static const NihDBusArg org_freedesktop_Avahi_Server_Browser_ItemNew_signal_args[] = {
	{ "interface", "i", NIH_DBUS_ARG_OUT },
	{ "protocol",  "i", NIH_DBUS_ARG_OUT },
	{ "name",      "s", NIH_DBUS_ARG_OUT },
	{ "type",      "s", NIH_DBUS_ARG_OUT },
	{ "domain",    "s", NIH_DBUS_ARG_OUT },
	{ "flags",     "u", NIH_DBUS_ARG_OUT },
	{ NULL }
};

static const NihDBusArg org_freedesktop_Avahi_Server_Browser_ItemRemove_signal_args[] = {
	{ "interface", "i", NIH_DBUS_ARG_OUT },
	{ "protocol",  "i", NIH_DBUS_ARG_OUT },
	{ "name",      "s", NIH_DBUS_ARG_OUT },
	{ "type",      "s", NIH_DBUS_ARG_OUT },
	{ "domain",    "s", NIH_DBUS_ARG_OUT },
	{ "flags",     "u", NIH_DBUS_ARG_OUT },
	{ NULL }
};

static const NihDBusArg org_freedesktop_Avahi_Server_Browser_AllForNow_signal_args[] = {
	{ NULL }
};

static DBusHandlerResult
org_freedesktop_Avahi_Server_Browser_ItemNew_signal (
		DBusConnection     *connection,
		DBusMessage        *signal,
		NihDBusProxySignal *proxied)
{
	NihDBusMessage *message;
	DBusError       error;
	int32_t         interface;
	int32_t         protocol;
	char           *name;
	char           *type;
	char           *domain;
	uint32_t        flags;

	nih_assert (connection != NULL);
	nih_assert (signal != NULL);
	nih_assert (proxied != NULL);
	nih_assert (connection == proxied->proxy->connection);

	if (! dbus_message_is_signal (signal, proxied->interface->name, proxied->signal->name))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (! dbus_message_has_path (signal, proxied->proxy->path))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (proxied->proxy->name)
		if (! dbus_message_has_sender (signal, proxied->proxy->owner))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	message = nih_dbus_message_new (NULL, connection, signal);
	if (! message)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Iterate the arguments to the signal and demarshal into arguments
	 * for our own function call.
	 */
	dbus_error_init (&error);

	if (! dbus_message_get_args(message->message, &error,
				    DBUS_TYPE_INT32,  &interface,
				    DBUS_TYPE_INT32,  &protocol,
				    DBUS_TYPE_STRING, &name,
				    DBUS_TYPE_STRING, &type,
				    DBUS_TYPE_STRING, &domain,
				    DBUS_TYPE_UINT32, &flags,
				    DBUS_TYPE_INVALID)) {
		dbus_error_is_set (&error);
		dbus_error_free (&error);
		nih_free (message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_free (&error);

	nih_error_push_context ();
	emit_event (dbus_message_get_member (signal), interface, protocol,
		    name, type, domain, flags);
	nih_error_pop_context ();

	nih_free (message);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
org_freedesktop_Avahi_Server_Browser_ItemRemove_signal (
		DBusConnection     *connection,
		DBusMessage        *signal,
		NihDBusProxySignal *proxied)
{
	NihDBusMessage *message;
	DBusError       error;
	int32_t         interface;
	int32_t         protocol;
	char *          name;
	char *          type;
	char *          domain;
	uint32_t        flags;

	nih_assert (connection != NULL);
	nih_assert (signal != NULL);
	nih_assert (proxied != NULL);
	nih_assert (connection == proxied->proxy->connection);

	if (! dbus_message_is_signal (signal, proxied->interface->name, proxied->signal->name))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (! dbus_message_has_path (signal, proxied->proxy->path))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (proxied->proxy->name)
		if (! dbus_message_has_sender (signal, proxied->proxy->owner))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	message = nih_dbus_message_new (NULL, connection, signal);
	if (! message)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Iterate the arguments to the signal and demarshal into arguments
	 * for our own function call.
	 */
	dbus_error_init (&error);

	if (! dbus_message_get_args(message->message, &error,
				    DBUS_TYPE_INT32,  &interface,
				    DBUS_TYPE_INT32,  &protocol,
				    DBUS_TYPE_STRING, &name,
				    DBUS_TYPE_STRING, &type,
				    DBUS_TYPE_STRING, &domain,
				    DBUS_TYPE_UINT32, &flags,
				    DBUS_TYPE_INVALID)) {
		dbus_error_is_set (&error);
		dbus_error_free (&error);
		nih_free (message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_free (&error);

	nih_error_push_context ();
	emit_event (dbus_message_get_member (signal), interface, protocol,
		    name, type, domain, flags);
	nih_error_pop_context ();

	nih_free (message);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
org_freedesktop_Avahi_Server_Browser_AllForNow_signal (
		DBusConnection     *connection,
		DBusMessage        *signal,
		NihDBusProxySignal *proxied)
{
	NihDBusMessage *message;
	DBusError       error;

	nih_assert (connection != NULL);
	nih_assert (signal != NULL);
	nih_assert (proxied != NULL);
	nih_assert (connection == proxied->proxy->connection);

	if (! dbus_message_is_signal (signal, proxied->interface->name, proxied->signal->name))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (! dbus_message_has_path (signal, proxied->proxy->path))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (proxied->proxy->name)
		if (! dbus_message_has_sender (signal, proxied->proxy->owner))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	message = nih_dbus_message_new (NULL, connection, signal);
	if (! message)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Iterate the arguments to the signal and demarshal into arguments
	 * for our own function call.
	 */
	dbus_error_init (&error);

	if (! dbus_message_get_args(message->message, &error,
				    DBUS_TYPE_INVALID)) {
		dbus_error_is_set (&error);
		dbus_error_free (&error);
		nih_free (message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_free (&error);

	nih_free (message);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// org.freedesktop.Avahi.ServiceBrowser
static const NihDBusSignal org_freedesktop_Avahi_ServiceBrowser_signals[] = {
	{ "ItemNew",        org_freedesktop_Avahi_Server_Browser_ItemNew_signal_args,        org_freedesktop_Avahi_Server_Browser_ItemNew_signal        },
	{ "ItemRemove",     org_freedesktop_Avahi_Server_Browser_ItemRemove_signal_args,     org_freedesktop_Avahi_Server_Browser_ItemRemove_signal     },
	{ "AllForNow",      org_freedesktop_Avahi_Server_Browser_AllForNow_signal_args,      org_freedesktop_Avahi_Server_Browser_AllForNow_signal      },
	{ NULL }
};

static const NihDBusProperty org_freedesktop_Avahi_ServiceBrowser_properties[] = {
	{ "version",      "s", NIH_DBUS_READ,      NULL, NULL },
	{ "log_priority", "s", NIH_DBUS_READWRITE, NULL, NULL },
	{ NULL }
};

const NihDBusInterface org_freedesktop_Avahi_ServiceBrowser = {
	"org.freedesktop.Avahi.ServiceBrowser",
	NULL,
	org_freedesktop_Avahi_ServiceBrowser_signals,
	org_freedesktop_Avahi_ServiceBrowser_properties
};

// org.freedesktop.Avahi.Server
static const NihDBusArg org_freedesktop_Avahi_Server_ServiceTypeBrowserNew_method_args[] = {
	{ "interface", "i", NIH_DBUS_ARG_OUT },
	{ "protocol",  "i", NIH_DBUS_ARG_OUT },
	{ "domain",    "s", NIH_DBUS_ARG_OUT },
	{ "flags",     "u", NIH_DBUS_ARG_OUT },
	{ NULL }
};

static const NihDBusArg org_freedesktop_Avahi_Server_ServiceBrowserNew_method_args[] = {
	{ "interface", "i", NIH_DBUS_ARG_OUT },
	{ "protocol",  "i", NIH_DBUS_ARG_OUT },
	{ "type",      "s", NIH_DBUS_ARG_OUT },
	{ "domain",    "s", NIH_DBUS_ARG_OUT },
	{ "flags",     "u", NIH_DBUS_ARG_OUT },
	{ NULL }
};

static const NihDBusMethod org_freedesktop_Avahi_Server_methods[] = {
	{ "ServiceTypeBrowserNew", org_freedesktop_Avahi_Server_ServiceTypeBrowserNew_method_args, NULL },
	{ "ServiceBrowserNew",     org_freedesktop_Avahi_Server_ServiceBrowserNew_method_args,     NULL },
	{ NULL }
};

static const NihDBusSignal org_freedesktop_Avahi_Server_signals[] = {
	{ "ItemNew",        org_freedesktop_Avahi_Server_Browser_ItemNew_signal_args,        org_freedesktop_Avahi_Server_Browser_ItemNew_signal        },
	{ "ItemRemove",     org_freedesktop_Avahi_Server_Browser_ItemRemove_signal_args,     org_freedesktop_Avahi_Server_Browser_ItemRemove_signal     },
	{ "AllForNow",      org_freedesktop_Avahi_Server_Browser_AllForNow_signal_args,      org_freedesktop_Avahi_Server_Browser_AllForNow_signal      },
	{ NULL }
};

static const NihDBusProperty org_freedesktop_Avahi_Server_properties[] = {
	{ "version",      "s", NIH_DBUS_READ,      NULL, NULL },
	{ "log_priority", "s", NIH_DBUS_READWRITE, NULL, NULL },
	{ NULL }
};

const NihDBusInterface org_freedesktop_Avahi_Server = {
	"org.freedesktop.Avahi.Server",
	org_freedesktop_Avahi_Server_methods,
	NULL,
	org_freedesktop_Avahi_Server_properties
};

int
main (int   argc,
      char *argv[])
{
	char           **args;
	DBusConnection *system_connection;
	DBusConnection *connection;
	DBusMessage    *message = NULL;
	DBusMessage    *reply = NULL;
	DBusError       error;
	int             ret;
	char           *pidfile_path = NULL;
	char           *pidfile = NULL;
	char           *path_element = NULL;

	nih_main_init (argv[0]);

	nih_option_set_synopsis (
		       _("Bridge avahi events into the user session upstart"));
	nih_option_set_help (
		 _("By default, upstart-avahi-bridge does not detach from the "
		   "console and remains in the foreground.  Use the --daemon "
		   "option to have it detach."));


	args = nih_option_parser (NULL, argc, argv, options, FALSE);
	if (! args)
		exit (1);

	/* Initialise the connection to Upstart */
	connection = NIH_SHOULD (nih_dbus_connect (DBUS_ADDRESS_UPSTART,
						   upstart_disconnected));

	if (! connection) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s", _("Could not connect to Upstart"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	upstart = NIH_SHOULD (nih_dbus_proxy_new (NULL, connection, NULL,
						  DBUS_PATH_UPSTART, NULL,
						  NULL));

	if (! upstart) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s", _("Could not create Upstart proxy"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	/* Initialise the connection to system Avahi */
	system_connection = NIH_SHOULD (nih_dbus_bus (DBUS_BUS_SYSTEM,
						      system_disconnected));

	if (! system_connection) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s", _("Could not connect to Upstart"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	avahi = NIH_SHOULD (nih_dbus_proxy_new (NULL, system_connection,
						"org.freedesktop.Avahi", "/",
						NULL, NULL));

	if (! avahi) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s", _("Could not create Avahi proxy"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	nih_list_init (&type_entries);
	nih_list_init (&browser_entries);
	avahi_type_browser = service_type_browser_new (avahi, -1, -1, "", 0);

	if (! avahi_type_browser) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			_("Could not create Avahi Service Type Browser proxy"),
			err->message);
		nih_free (err);

		exit (1);
	}

	if (! nih_dbus_proxy_connect (avahi_type_browser,
			  &org_freedesktop_Avahi_ServiceTypeBrowser,
			  "ItemNew",
			  (NihDBusSignalHandler)upstart_forward_event, NULL)) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			   _("Could not create ItemNew signal connection"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	if (! nih_dbus_proxy_connect (avahi_type_browser,
			  &org_freedesktop_Avahi_ServiceTypeBrowser,
			  "ItemRemove",
			  (NihDBusSignalHandler)upstart_forward_event, NULL)) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			   _("Could not create ItemRemove signal connection"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	if (! nih_dbus_proxy_connect (avahi_type_browser,
			  &org_freedesktop_Avahi_ServiceTypeBrowser,
			  "AllForNow",
			  (NihDBusSignalHandler)upstart_forward_event, NULL)) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			   _("Could not create AllForNow signal connection"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	/* Become daemon */
	if (daemonise) {
		if (nih_main_daemonise () < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_fatal ("%s: %s", _("Unable to become daemon"),
				   err->message);
			nih_free (err);

			exit (1);
		}

		/* Send all logging output to syslog */
		openlog (program_name, LOG_PID, LOG_DAEMON);
		nih_log_set_logger (nih_logger_syslog);
	}

	/* Handle TERM and INT signals gracefully */
	nih_signal_set_handler (SIGTERM, nih_signal_handler);
	NIH_MUST (nih_signal_add_handler (NULL, SIGTERM, nih_main_term_signal, NULL));

	if (! daemonise) {
		nih_signal_set_handler (SIGINT, nih_signal_handler);
		NIH_MUST (nih_signal_add_handler (NULL, SIGINT, nih_main_term_signal, NULL));
	}

	ret = nih_main_loop ();

	NIH_LIST_FOREACH_SAFE (&browser_entries, iter) {
		NihListEntry *browser_entry = (NihListEntry *)iter;
		NihDBusProxy *proxy = (NihDBusProxy *)browser_entry->data;
		nih_list_remove (&browser_entry->entry);
		nih_free (browser_entry);

		service_browser_free (proxy);
		nih_free (proxy);
	}

	if (avahi_type_browser) {
		service_type_browser_free (avahi_type_browser);
		nih_free (avahi_type_browser);
		avahi_type_browser = NULL;
	}

	if (avahi) {
		nih_free (avahi);
		avahi = NULL;
	}

	dbus_connection_unref (connection);

	if (upstart) {
		if (upstart->path) {
			nih_free (upstart->path);
		}

		if (upstart->name) {
			nih_free (upstart->name);
		}

		nih_free (upstart);
	}

	if (package_string) {
		nih_free ((void *) package_string);
		package_string = NULL;
	}

	if (nih_main_loop_functions) {
//		NIH_LIST_FOREACH_SAFE (&nih_main_loop_functions, iter) {
//			NihListEntry *browser_entry = (NihListEntry *)iter;
//			NihDBusProxy *proxy = (NihDBusProxy *)browser_entry->data;
//			nih_list_remove (&browser_entry->entry);
//			nih_free (browser_entry);
//
//			service_browser_free (proxy);
//			nih_free (proxy);
//		}
		nih_free (nih_main_loop_functions);
		nih_main_loop_functions = NULL;
	}

	if (args) {
		nih_free (args);
	}

	return ret;
}

static void
upstart_disconnected (DBusConnection *connection)
{
	nih_fatal (_("Disconnected from Upstart"));
	nih_main_loop_exit (1);
}

static void
system_disconnected (DBusConnection *connection)
{
	nih_fatal (_("Disconnected from System"));
	nih_main_loop_exit (1);
}

static void
upstart_forward_event (void           *data,
		       NihDBusMessage *message,
		       const char     *path)
{
	char            *event_name = NULL;
	nih_local char  *new_event_name = NULL;
	char           **event_env = NULL;
	int              event_env_count = 0;
	DBusError        error;
	DBusPendingCall *pending_call;

	dbus_error_init (&error);

	/* Extract information from the original event */
	if (!dbus_message_get_args (message->message, &error,
				    DBUS_TYPE_STRING, &event_name,
				    DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
				    &event_env, &event_env_count,
				    DBUS_TYPE_INVALID)) {
		nih_error ("DBUS error: %s", error.message);
		dbus_error_free (&error);
		return;
	}

	nih_assert (event_name != NULL);

	/* Build the new event name */
	NIH_MUST (nih_strcat_sprintf (&new_event_name, NULL, ":sys:%s",
				      event_name));

	/* Re-transmit the event */
	pending_call = upstart_emit_event (upstart, new_event_name, event_env,
					   FALSE, NULL, emit_event_error, NULL,
					   NIH_DBUS_TIMEOUT_NEVER);

	if (! pending_call) {
		NihError *err;
		err = nih_error_get ();
		nih_warn ("%s", err->message);
		nih_free (err);
	}

	dbus_pending_call_unref (pending_call);
	dbus_free_string_array (event_env);
}

static void
emit_event_error (void           *data,
		  NihDBusMessage *message)
{
	NihError *err;

	err = nih_error_get ();
	nih_warn ("%s", err->message);
	nih_free (err);
}

static NihDBusProxy *
service_type_browser_new (NihDBusProxy *proxy,
			  int32_t       interface,
			  int32_t       protocol,
			  const char   *domain,
			  uint32_t      flags)
{
	NihDBusProxy   *ret = NULL;
	DBusMessage    *method_call;
	DBusError       error;
	DBusMessage    *reply;
	const char     *property;
	const char     *local_dbus;
	char           *local;
	char           *string;

	nih_assert (proxy != NULL);

	/* Construct the method call message. */
	method_call = dbus_message_new_method_call (proxy->name, proxy->path,
						"org.freedesktop.Avahi.Server",
						"ServiceTypeBrowserNew");

	if (! method_call)
		nih_return_no_memory_error (NULL);

	dbus_message_set_auto_start (method_call, proxy->auto_start);

	if (! dbus_message_append_args (method_call,
					DBUS_TYPE_INT32,  &interface,
					DBUS_TYPE_INT32,  &protocol,
					DBUS_TYPE_STRING, &domain,
					DBUS_TYPE_UINT32, &flags,
					DBUS_TYPE_INVALID)) {
		dbus_message_unref (method_call);
		nih_return_no_memory_error (NULL);
	}

	/* Send the message, and wait for the reply. */
	dbus_error_init (&error);

	reply = dbus_connection_send_with_reply_and_block (proxy->connection,
							   method_call, -1,
							   &error);

	if (! reply) {
		dbus_message_unref (method_call);

		if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY)) {
			nih_error_raise_no_memory ();
		} else {
			nih_dbus_error_raise (error.name, error.message);
		}

		dbus_error_free (&error);
		return NULL;
	}

	dbus_message_unref (method_call);

	/* Iterate the arguments to the method call and demarshal into
	 * arguments for our own function call.
	 */
	if (!dbus_message_get_args (reply, &error,
				    DBUS_TYPE_OBJECT_PATH, &string,
				    DBUS_TYPE_INVALID)) {
		dbus_message_unref (reply);
		nih_return_error (NULL, NIH_DBUS_INVALID_ARGS,
		                  _(NIH_DBUS_INVALID_ARGS_STR));
	}

	dbus_message_unref (reply);

	ret = NIH_SHOULD (nih_dbus_proxy_new (NULL, proxy->connection,
						"org.freedesktop.Avahi", string,
						NULL, NULL));

	return ret;
}

static int
service_type_browser_free (NihDBusProxy *proxy)
{
	DBusMessage    *method_call;
	DBusError       error;
	DBusMessage    *reply;

	nih_assert (proxy != NULL);

	/* Construct the method call message. */
	method_call = dbus_message_new_method_call (proxy->name, proxy->path,
				    "org.freedesktop.Avahi.ServiceTypeBrowser",
				    "Free");

	if (! method_call)
		nih_return_no_memory_error (-1);

	dbus_message_set_auto_start (method_call, proxy->auto_start);

	/* Send the message, and wait for the reply. */
	dbus_error_init (&error);

	reply = dbus_connection_send_with_reply_and_block (proxy->connection,
							   method_call, -1,
							   &error);

	if (! reply) {
		dbus_message_unref (method_call);

		if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY)) {
			nih_error_raise_no_memory ();
		} else {
			nih_dbus_error_raise (error.name, error.message);
		}

		dbus_error_free (&error);
		return -1;
	}

	dbus_message_unref (method_call);

	/* Iterate the arguments to the method call and demarshal into
	 * arguments for our own function call.
	 */
	if (!dbus_message_get_args (reply, &error,
				    DBUS_TYPE_INVALID)) {
		dbus_message_unref (reply);
		nih_return_error (NULL, NIH_DBUS_INVALID_ARGS,
		                  _(NIH_DBUS_INVALID_ARGS_STR));
	}

	dbus_message_unref (reply);

	return 0;
}

static NihDBusProxy *
service_browser_new (NihDBusProxy *proxy,
		     int32_t       interface,
		     int32_t       protocol,
		     const char   *type,
		     const char   *domain,
		     uint32_t      flags)
{
	NihDBusProxy   *ret = NULL;
	DBusMessage    *method_call;
	DBusError       error;
	DBusMessage    *reply;
	const char     *property;
	const char     *local_dbus;
	char           *local;
	char           *string;

	nih_assert (proxy != NULL);

	/* Construct the method call message. */
	method_call = dbus_message_new_method_call (proxy->name, proxy->path,
						"org.freedesktop.Avahi.Server",
						"ServiceBrowserNew");

	if (! method_call)
		nih_return_no_memory_error (NULL);

	dbus_message_set_auto_start (method_call, proxy->auto_start);

	if (! dbus_message_append_args (method_call,
					DBUS_TYPE_INT32,  &interface,
					DBUS_TYPE_INT32,  &protocol,
					DBUS_TYPE_STRING, &type,
					DBUS_TYPE_STRING, &domain,
					DBUS_TYPE_UINT32, &flags,
					DBUS_TYPE_INVALID)) {
		dbus_message_unref (method_call);
		nih_return_no_memory_error (NULL);
	}

	/* Send the message, and wait for the reply. */
	dbus_error_init (&error);

	reply = dbus_connection_send_with_reply_and_block (proxy->connection,
							   method_call, -1,
							   &error);

	if (! reply) {
		dbus_message_unref (method_call);

		if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY)) {
			nih_error_raise_no_memory ();
		} else {
			nih_dbus_error_raise (error.name, error.message);
		}

		dbus_error_free (&error);
		return NULL;
	}

	dbus_message_unref (method_call);

	/* Iterate the arguments to the method call and demarshal into
	 * arguments for our own function call.
	 */
	if (!dbus_message_get_args (reply, &error,
				    DBUS_TYPE_OBJECT_PATH, &string,
				    DBUS_TYPE_INVALID)) {
		dbus_message_unref (reply);
		nih_return_error (NULL, NIH_DBUS_INVALID_ARGS,
		                  _(NIH_DBUS_INVALID_ARGS_STR));
	}

	dbus_message_unref (reply);

	ret = NIH_SHOULD (nih_dbus_proxy_new (NULL, proxy->connection,
					      "org.freedesktop.Avahi", string,
					      NULL, NULL));

	return ret;
}

static int
service_browser_free (NihDBusProxy *proxy)
{
	DBusMessage    *method_call;
	DBusError       error;
	DBusMessage    *reply;

	nih_assert (proxy != NULL);

	/* Construct the method call message. */
	method_call = dbus_message_new_method_call (proxy->name, proxy->path,
					"org.freedesktop.Avahi.ServiceBrowser",
					"Free");

	if (! method_call)
		nih_return_no_memory_error (-1);

	dbus_message_set_auto_start (method_call, proxy->auto_start);

	/* Send the message, and wait for the reply. */
	dbus_error_init (&error);

	reply = dbus_connection_send_with_reply_and_block (proxy->connection,
							   method_call, -1,
							   &error);

	if (! reply) {
		dbus_message_unref (method_call);

		if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY)) {
			nih_error_raise_no_memory ();
		} else {
			nih_dbus_error_raise (error.name, error.message);
		}

		dbus_error_free (&error);
		return -1;
	}

	dbus_message_unref (method_call);

	/* Iterate the arguments to the method call and demarshal into
	 * arguments for our own function call.
	 */
	if (!dbus_message_get_args (reply, &error,
				    DBUS_TYPE_INVALID)) {
		dbus_message_unref (reply);
		nih_return_error (NULL, NIH_DBUS_INVALID_ARGS,
		                  _(NIH_DBUS_INVALID_ARGS_STR));
	}

	dbus_message_unref (reply);

	return 0;
}

static int
emit_event (const char *action,
	    int32_t     interface,
	    int32_t     protocol,
	    const char *name,
	    const char *type,
	    const char *domain,
	    uint32_t    flags)
{
	DBusPendingCall *pending_call;
	const char      *event_name = NULL;
	nih_local char **env = NULL;
	size_t           env_len = 0;
	char            ifname[IF_NAMESIZE];

	if (! strcmp (action, "ItemNew")) {
		event_name = "net-service-up";
	} else if (! strcmp (action, "ItemRemove")) {
		event_name = "net-service-down";
	} else {
		return -1;
	}

	env = NIH_MUST (nih_str_array_new (NULL));

	if (interface != -1)
	{
		nih_local char *var = NULL;

		var = NIH_MUST (nih_sprintf (NULL, "IFACE=%s",
					  if_indextoname (interface, ifname)));
		NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
	}

	if (protocol != -1)
	{
		nih_local char *var = NULL;

		var = NIH_MUST (nih_sprintf (NULL, "PROTO=%s",
					     protocol == 0 ? "IPv4" : "IPv6"));
		NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
	}

	if (name)
	{
		nih_local char *var = NULL;

		var = NIH_MUST (nih_sprintf (NULL, "NAME=%s", name));
		NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
	}

	if (domain)
	{
		nih_local char *var = NULL;

		var = NIH_MUST (nih_sprintf (NULL, "DOMAIN=%s", domain));
		NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
	}

	if (type)
	{
		nih_local char *var = NULL;

		var = NIH_MUST (nih_sprintf (NULL, "TYPE=%s", type));
		NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
	}

	nih_debug ("%s IFACE=%s PROTO=%s NAME=%s DOMAIN=%s TYPE=%s",
		   event_name,
		   interface == -1 ? "-1" : ifname,
		   protocol == 0 ? "IPv4" : "IPv6",
		   name,
		   domain,
		   type);

	pending_call = upstart_emit_event (upstart, event_name, env, FALSE,
					   NULL, emit_event_error, NULL,
					   NIH_DBUS_TIMEOUT_NEVER);

	if (! pending_call) {
		NihError *err;
		int       saved = errno;

		err = nih_error_get ();
		nih_warn ("%s", err->message);

		nih_free (err);
	}

	dbus_pending_call_unref (pending_call);

	return 0;
}

NihDBusProxy *
avahi_create_browser_proxy (const char *type,
			    const char *domain)
{
	NihListEntry *entry;
	NihDBusProxy *proxy;

	proxy = service_browser_new (avahi, -1, -1, type, domain, 0);

	if (! proxy) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			   _("Could not create Avahi Service Browser proxy"),
			   err->message);
		nih_free (err);

		return NULL;
	}

	if (! nih_dbus_proxy_connect (proxy,
			  &org_freedesktop_Avahi_ServiceBrowser,
			  "ItemNew",
			  (NihDBusSignalHandler)upstart_forward_event, NULL)) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			   _("Could not create ItemNew signal connection"),
			   err->message);
		nih_free (err);
		nih_free (proxy);

		return NULL;
	}

	if (! nih_dbus_proxy_connect (proxy,
			  &org_freedesktop_Avahi_ServiceBrowser,
			  "ItemRemove",
			  (NihDBusSignalHandler)upstart_forward_event, NULL)) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			    _("Could not create ItemRemove signal connection"),
			    err->message);
		nih_free (err);
		nih_free (proxy);

		return NULL;
	}

	if (! nih_dbus_proxy_connect (proxy,
			  &org_freedesktop_Avahi_ServiceBrowser,
			  "AllForNow",
			  (NihDBusSignalHandler)upstart_forward_event, NULL)) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s",
			   _("Could not create AllForNow signal connection"),
			   err->message);
		nih_free (err);
		nih_free (proxy);

		return NULL;
	}

	entry = NIH_MUST (nih_list_entry_new (NULL));
	entry->data = proxy;
	nih_list_add (&browser_entries, &entry->entry);

	return proxy;
}
