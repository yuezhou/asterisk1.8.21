/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Echo application -- play back what you hear to evaluate latency
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 360033 $")

#include "asterisk/file.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/channel.h"
#include "asterisk/options.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/astobj2.h"

/*** DOCUMENTATION
	<application name="Echo" language="en_US">
		<synopsis>
			Echo media, DTMF back to the calling party
		</synopsis>
		<syntax />
		<description>
			<para>Echos back any media or DTMF frames read from the calling 
			channel back to itself. This will not echo CONTROL, MODEM, or NULL
			frames. Note: If '#' detected application exits.</para>
			<para>This application does not automatically answer and should be
			preceeded by an application such as Answer() or Progress().</para>
		</description>
	</application>
 ***/

#define BUFFER_SIZE 200

static const char app[] = "Echo";

static int echo_exec(struct ast_channel *chan, const char *data)
{
	AST_DECLARE_APP_ARGS(args, 
		AST_APP_ARG(options);
	);

	typedef enum {
		OPT_DELAY = (1 << 0),
	} echo_opt_flags;

	typedef enum {
		OPT_ARG_DELAY,
		OPT_ARG_ARRAY_SIZE,
	} echo_opt_args;

	AST_APP_OPTIONS(echo_opts, {
		AST_APP_OPTION_ARG('d', OPT_DELAY, OPT_ARG_DELAY),
	});
	char *parse;
	int res = -1;
	format_t format;
	struct ast_flags opts = {0};
	char *opt_args[OPT_ARG_ARRAY_SIZE];
	ast_verb(3, "Echo initiated\n");
	int data_size = 0;
	int read_pointer = 1;
	int write_pointer = 0;
	int i;
	int delayed = 0;
	int delay_seconds = 0;
	int delay_frames = 0;
	struct ast_frame **buffer = NULL;

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	if (!ast_strlen_zero(args.options) && ast_app_parse_options(echo_opts, &opts, opt_args, args.options))
	{
		ast_verb(3, "No delay option\n");
	}
	if (ast_test_flag(&opts, OPT_DELAY) && !ast_strlen_zero(opt_args[OPT_ARG_DELAY]))
	{
		delay_seconds = atoi(opt_args[OPT_ARG_DELAY]);
		if (delay_seconds < 0) delay_seconds = 0;
		delay_frames = 50*delay_seconds;
		delayed = 1;
		ast_verb(3, "Delay Secs: %d\n", delay_seconds);
	}
	
	if (delayed)
	{
		buffer = malloc(delay_frames*sizeof(struct ast_frame *));
		memset(buffer, 0, delay_frames*sizeof(struct ast_frame *));
	}

	format = ast_best_codec(chan->nativeformats);
	ast_set_write_format(chan, format);
	ast_set_read_format(chan, format);

	while (ast_waitfor(chan, -1) > -1) {
		struct ast_frame *f = ast_read(chan);
		if (!f) {
			break;
		}
		
		if (++write_pointer >= delay_frames) write_pointer = 0;		

		f->delivery.tv_sec = 0;
		f->delivery.tv_usec = 0;
		if (!delayed)
		{
			if (ast_write(chan, f)) {
				ast_frfree(f);
				goto end;
			}
			if ((f->frametype == AST_FRAME_DTMF) && (f->subclass.integer == '#')) {
				res = 0;
				ast_frfree(f);
				goto end;
			}
		}
		if (f->frametype != AST_FRAME_CONTROL
			&& f->frametype != AST_FRAME_MODEM
			&& f->frametype != AST_FRAME_NULL
			&& ast_write(chan, f)) {
			ast_frfree(f);
		}
		else
		{
			buffer[write_pointer] = ast_frisolate(f);
			if (data_size < delay_frames) data_size++;
			
			if (data_size >= delay_frames)
			{
				if (++read_pointer >= delay_frames) read_pointer = 0;
				if (ast_write(chan, buffer[read_pointer])) {
					ast_frfree(buffer[read_pointer]);
					buffer[read_pointer] = NULL;
					goto end;
				} else {
					ast_frfree(buffer[read_pointer]);
					buffer[read_pointer] = NULL;
				}
			}
		}
	}
end:
	if (delayed)
	{
		for (i = 0; i < delay_frames; i++) {
			if (buffer[i]) {
				ast_frfree(buffer[i]);
			}
		}
	}
	return res;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application_xml(app, echo_exec);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Simple Echo Application");
