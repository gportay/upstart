/* upstart
 *
 * Copyright © 2013 Canonical Ltd.
 * Author: Gaël Portay <gael.portay@gmail.com>.
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <ctype.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/netlink.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/io.h>
#include <nih/option.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <nih/error.h>

#include <nih-dbus/dbus_connection.h>
#include <nih-dbus/dbus_proxy.h>

#include "dbus/upstart.h"
#include "com.ubuntu.Upstart.h"


struct nl_context_t {
	int                  fd;
	struct sockaddr_nl   addr;
	struct msghdr        msg;
};

/* Prototypes for static functions */
static void netlink_monitor_watcher (struct nl_context_t *context,
				  NihIoWatch *watch, NihIoEvents events);
static void upstart_disconnected (DBusConnection *connection);
static void emit_event_error     (void *data, NihDBusMessage *message);

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
 * options:
 *
 * Command-line options accepted by this program.
 **/
static NihOption options[] = {
	{ 0, "daemon", N_("Detach and run in the background"),
	  NULL, NULL, &daemonise, NULL },

	NIH_OPTION_LAST
};


int
main (int   argc,
      char *argv[])
{
	char **              args;
	DBusConnection *     connection;
	struct nl_context_t  nlctx;
	int                  ret;

	nih_main_init (argv[0]);

	nih_option_set_synopsis (_("Bridge uevent kobjects into upstart"));
	nih_option_set_help (
		_("By default, upstart-uevent-bridge does not detach from the "
		  "console and remains in the foreground.  Use the --daemon "
		  "option to have it detach."));

	args = nih_option_parser (NULL, argc, argv, options, FALSE);
	if (! args)
		exit (1);

	/* Initialise the connection to Upstart */
	connection = NIH_SHOULD (nih_dbus_connect (DBUS_ADDRESS_UPSTART, upstart_disconnected));
	if (! connection) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s", _("Could not connect to Upstart"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	upstart = NIH_SHOULD (nih_dbus_proxy_new (NULL, connection,
						  NULL, DBUS_PATH_UPSTART,
						  NULL, NULL));
	if (! upstart) {
		NihError *err;

		err = nih_error_get ();
		nih_fatal ("%s: %s", _("Could not create Upstart proxy"),
			   err->message);
		nih_free (err);

		exit (1);
	}

	/* Initialise the netlink socket */
	memset(&nlctx.addr, 0, sizeof(nlctx.addr));
	nlctx.addr.nl_family = AF_NETLINK;
	nlctx.addr.nl_groups = NETLINK_KOBJECT_UEVENT;

	nih_assert ((nlctx.fd = socket(PF_NETLINK, SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT)) >= 0);
	nih_assert (bind(nlctx.fd, (struct sockaddr *) &nlctx.addr, sizeof(nlctx.addr)) == 0);

	NIH_MUST (nih_io_add_watch (NULL, nlctx.fd,
				    NIH_IO_READ,
				    (NihIoWatcher)netlink_monitor_watcher,
				    (void *)&nlctx));

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

	return ret;
}


static void
netlink_monitor_watcher (struct nl_context_t *context,
		      NihIoWatch *         watch,
		      NihIoEvents          events)
{
	char                    buf[4096];
	char *                  n = NULL;
	char *                  s = NULL;
	struct iovec            iov = { buf, sizeof(buf) };
	ssize_t                 len;
	struct nlmsghdr *       nh;
	char *                  subsystem = NULL;
	char *                  subsystem_var = NULL;
	char *                  action = NULL;
	char *                  action_var = NULL;
	char *                  kernel = NULL;
	char *                  kernel_var = NULL;
	char *                  devpath = NULL;
	char *                  devpath_var = NULL;
	char *                  devname = NULL;
	char *                  devname_var = NULL;
	char *                  name = NULL;
	char *                  name_var = NULL;
	nih_local char **       env = NULL;
	const char *            value = NULL;
	size_t                  env_len = 0;
	DBusPendingCall *       pending_call;
	
	
	context->msg.msg_name = &context->addr;
	context->msg.msg_namelen = sizeof(context->addr);
	context->msg.msg_iov = &iov;
	context->msg.msg_iovlen = 1;
	context->msg.msg_control = NULL;
	context->msg.msg_controllen = 0;
	context->msg.msg_flags = 0;

	while (len = recvmsg(context->fd, &context->msg, 0)) {
		unsigned long int initialized = 0;

		if (len == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
			break;
		}
		
		if (len == -1) {
			perror("recvmsg");
			break;
		}

		s = buf;
		if (! memcmp(buf, "libudev", sizeof("libudev"))) {
			unsigned int *magic = (unsigned int *) &buf[sizeof("libudev")];
			if (htonl(*magic) != 0xfeedcafe) {
				printf("magic: %x != %x\n", htonl(*magic), 0xfeedcafe);
				return;
			}
			unsigned int *prop_offset = (unsigned int *) &buf[sizeof("libudev") + 2 * sizeof(uint32_t)];
			s += *prop_offset;
		}
		else {
			size_t l = strlen (s);
			s += l + 1;
		}
	
		while ((n = strchr(s, 0)) && n < &buf[len]) {
			char *k = NULL;
			char *v = NULL;
			
			n++;
			k = s;
			s = n;
			if ((v = strchr(k, '=')) == NULL) {
				break;
			}
			v++;
		
			if (! strncmp (k, "ACTION", sizeof("ACTION") - 1)) {
				action = v;
				action_var = k;
			} else if (! strncmp (k, "KERNEL", sizeof("KERNEL") - 1)) {
				kernel = v;
				kernel_var = k;
			} else if (! strncmp (k, "DEVPATH", sizeof("DEVPATH") - 1)) {
				devpath = v;
				devpath_var = k;
			} else if (! strncmp (k, "DEVNAME", sizeof("DEVNAME") - 1)) {
				devname = v;
				devname_var = k;
			} else if (! strncmp (k, "SUBSYSTEM", sizeof("SUBSYSTEM") - 1)) {
				subsystem = v;
				subsystem_var = k;
			} else if (! strncmp (k, "USEC_INITIALIZED", sizeof("USEC_INITIALIZED") - 1)) {
				initialized = strtol (v, NULL, 0);
			}
			
			nih_debug ("%s\n", s);
		}

		if (! action) {
			return;
		}

		if (! initialized) {
			return;
		}
	
		if (! strcmp (action, "add")) {
			name = NIH_MUST (nih_sprintf (NULL, "%s-device-added",
							  subsystem));
		} else if (! strcmp (action, "change")) {
			name = NIH_MUST (nih_sprintf (NULL, "%s-device-changed",
							  subsystem));
		} else if (! strcmp (action, "remove")) {
			name = NIH_MUST (nih_sprintf (NULL, "%s-device-removed",
							  subsystem));
		} else {
			name = NIH_MUST (nih_sprintf (NULL, "%s-device-%s",
							  subsystem, action));
						  }
	
		env = NIH_MUST (nih_str_array_new (NULL));

		if (kernel) {
			nih_local char *var = NULL;

			var = NIH_MUST (nih_strdup (NULL, kernel_var));
			NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
		}

		if (devpath) {
			nih_local char *var = NULL;

			var = NIH_MUST (nih_strdup (NULL, devpath_var));
			NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
		}
		else {
			continue;
		}

		if (devname) {
			nih_local char *var = NULL;

			var = NIH_MUST (nih_strdup (NULL, devname_var));
			NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
		}

		if (subsystem) {
			nih_local char *var = NULL;

			var = NIH_MUST (nih_strdup (NULL, subsystem_var));
			NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
		}
		else {
			continue;
		}

		if (action) {
			nih_local char *var = NULL;

			var = NIH_MUST (nih_strdup (NULL, action_var));
			NIH_MUST (nih_str_array_addp (&env, NULL, &env_len, var));
		}
		else {
			continue;
		}

		nih_debug ("action: %s, devpath: %s, devname: %s, subsystem: %s, kernel: %s\n", action, devpath, devname, subsystem, kernel);

		pending_call = upstart_emit_event (upstart,
			name, env, FALSE,
				NULL, emit_event_error, NULL,
				NIH_DBUS_TIMEOUT_NEVER);

		if (! pending_call) {
			NihError *err;
			int saved = errno;

			err = nih_error_get ();
			nih_warn ("%s", err->message);

			if (saved != ENOMEM && subsystem)
				nih_warn ("Likely that uevent '%s' event contains binary garbage", subsystem);

			nih_free (err);
		}

		dbus_pending_call_unref (pending_call);
	}
}

static void
upstart_disconnected (DBusConnection *connection)
{
	nih_fatal (_("Disconnected from Upstart"));
	nih_main_loop_exit (1);
}

static void
emit_event_error (void *          data,
		  NihDBusMessage *message)
{
	NihError *err;

	err = nih_error_get ();
	nih_warn ("%s", err->message);
	nih_free (err);
}
