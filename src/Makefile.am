MAINTAINERCLEANFILES = Makefile.in config.h config.h.in
DISTCLEANFILES = Makefile
CLEANFILES = core.* *~ 
AM_CFLAGS = -Wall
sexec_CPPFLAGS = -DSYSCONFDIR=\"$(sysconfdir)\" -DLOCALSTATEDIR=\"$(localstatedir)\" -DLIBEXECDIR=\"$(libexecdir)\" $(NAMESPACE_DEFINES)
bootstrap_CPPFLAGS = -DLIBEXECDIR=\"$(libexecdir)\"
ftrace_CPPFLAGS = -DARCH_$(SINGULARITY_ARCH)

dist_suidPROGRAM_INSTALL = ${INSTALL} -m 640

install-exec-hook:
	@if test "$$UID" = "0"; then \
		echo " /bin/chown root:root $(DESTDIR)$(libexecdir)/singularity/sexec"; \
		/bin/chown root:root $(DESTDIR)$(libexecdir)/singularity/sexec; \
		echo " /bin/chmod 4755 $(DESTDIR)$(libexecdir)/singularity/sexec"; \
		/bin/chmod 4755 $(DESTDIR)$(libexecdir)/singularity/sexec; \
	fi

bindir = $(libexecdir)/singularity
bin_PROGRAMS = ftrace ftype sexec mount bootstrap

ftrace_SOURCES = ftrace.c util.c util.h
ftype_SOURCES = ftype.c util.c util.h
sexec_SOURCES = sexec.c util.c util.h loop-control.c loop-control.h mounts.c mounts.h user.c user.h
mount_SOURCES = mount.c util.c util.h loop-control.c loop-control.h mounts.c mounts.h
bootstrap_SOURCES = bootstrap.c util.c util.h loop-control.c loop-control.h mounts.c mounts.h

EXTRA_DIST = config.h 
