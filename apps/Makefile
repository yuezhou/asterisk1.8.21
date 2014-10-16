#
# Asterisk -- A telephony toolkit for Linux.
# 
# Makefile for PBX applications
#
# Copyright (C) 1999-2006, Digium, Inc.
#
# This program is free software, distributed under the terms of
# the GNU General Public License
#

-include $(ASTTOPDIR)/menuselect.makeopts $(ASTTOPDIR)/menuselect.makedeps

MODULE_PREFIX=app
MENUSELECT_CATEGORY=APPS
MENUSELECT_DESCRIPTION=Applications

MENUSELECT_OPTS_app_directory:=$(MENUSELECT_OPTS_app_voicemail)
ifneq ($(findstring ODBC_STORAGE,$(MENUSELECT_OPTS_app_voicemail)),)
  MENUSELECT_DEPENDS_app_voicemail+=$(MENUSELECT_DEPENDS_ODBC_STORAGE)
endif
ifneq ($(findstring IMAP_STORAGE,$(MENUSELECT_OPTS_app_voicemail)),)
  MENUSELECT_DEPENDS_app_voicemail+=$(MENUSELECT_DEPENDS_IMAP_STORAGE)
endif

MENUSELECT_DEPENDS_app_dial+=SIT
MENUSELECT_DEPENDS_app_playback2+=TONE
MENUSELECT_DEPENDS_app_playback2+=POCKETSPHINX
MENUSELECT_DEPENDS_app_waitfortoneorsilence+=TONE
TONE_LIB=tone_detect/tone_detect.a
POCKETSPHINX_LIB=${POCKETSPHINX_LIB}
SIT_LIB=sit_detect/sit_detect.a


ifeq (SunOS,$(shell uname))
MENUSELECT_DEPENDS_app_chanspy+=RT
RT_LIB=-lrt
endif

all: _all sit_detect tone_detect

include $(ASTTOPDIR)/Makefile.moddir_rules

ifneq ($(findstring $(OSARCH), mingw32 cygwin ),)
  LIBS+= -lres_features.so -lres_ael_share.so -lres_monitor.so -lres_speech.so
  LIBS+= -lres_smdi.so
endif

