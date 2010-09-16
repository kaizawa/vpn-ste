#
# Copyright (C) 2004-2010 Kazuyoshi Aizawa. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#   notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer in the
#   documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#

# Makefile for ste
CC = gcc
KCFLAGS = -O -D_KERNEL -D_SYSCALL32 -m64 -mcmodel=kernel -mno-red-zone -DSOL10
DRV_PATH = /usr/kernel/drv/amd64
DRV_CONF_PATH = /usr/kernel/drv
PRODUCTS = ste sted  stehub
CFLAGS = -g -O2
ECHO = /bin/echo
CP = /bin/cp
RM = /bin/rm
LD = /usr/ccs/bin/ld
RM = /bin/rm
CAT = /bin/cat
INSTALL = /usr/sbin/install
REM_DRV = /usr/sbin/rem_drv 
ADD_DRV = /usr/sbin/add_drv 

all: $(PRODUCTS)

clean:
	$(RM) -f *.o sted stehub ste

ste.o: ste.c ste.h
	$(CC) -c $(KCFLAGS) $< -o $@

ste: ste.o
	$(LD) -dn -r $^ -o $@

stehub.o: stehub.c sted.h ste.h
	$(CC) -c $(CFLAGS) $< -o $@

stehub: stehub.o
	$(CC) $(CFLAGS) -lsocket -lnsl $^ -o $@

sted.o: sted.c ste.h sted.h dlpiutil.h
	$(CC) -c $(CFLAGS) $< -o $@

sted_socket.o: sted_socket.c sted_socket.c
	$(CC) -c $(CFLAGS) $< -o $@

dlpiutil.o: dlpiutil.c dlpiutil.h
	$(CC) -c $(CFLAGS) $< -o $@

sted: sted.o sted_socket.o dlpiutil.o
	$(CC) $(CFLAGS) -lsocket -lnsl $^ -o $@

install: all
	-$(INSTALL) -s -f $(DRV_PATH) -m 0755 -u root -g sys ste
	-$(INSTALL) -s -f /kernel/drv -m 0644 -u root -g sys ste.conf
	$(INSTALL) -s -d /usr/local/bin 
	-$(INSTALL) -s -f /usr/local/bin -m 0755 -u root sted 
	-$(INSTALL) -s -f /usr/local/bin -m 0755 -u root stehub
	$(ADD_DRV) ste

uninstall:
	-$(REM_DRV) ste
	-$(RM) $(DRV_PATH)/ste
	-$(RM) /kernel/drv/ste.conf
	-$(RM) /usr/local/bin/sted
	-$(RM) /usr/local/bin/stehub
