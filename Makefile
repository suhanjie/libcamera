all: libcamera

BUILDDIR=build
INSTALLDIR=/tmp/install/libcamera/

$(BUILDDIR):
	@meson $(BUILDDIR)

libcamera: $(BUILDDIR)
	@cd $(BUILDDIR) && ninja

distclean:
	rm -r $(BUILDDIR)

Documentation/linkcheck clean test: .PHONY
	@cd $(BUILDDIR) && ninja $@

linkcheck: Documentation/linkcheck

gitlab-ci:
	gitlab-runner exec docker build

build-test: distclean all test

doc: libcamera
	xdg-open $(BUILDDIR)/Documentation/html/index.html
	xdg-open $(BUILDDIR)/Documentation/api-html/index.html

install:
	rm -rf $(INSTALLDIR)
	cd $(BUILDDIR) && \
		DESTDIR=$(INSTALLDIR) ninja install
	tree $(INSTALLDIR)

commands:
	cd $(BUILDDIR) && ninja -t commands

v4l2device: libcamera
	$(BUILDDIR)/test/v4l2_device

update-gitlab:
	cd gitlab \
		&& git fetch linuxtv.org \
		&& git rebase linuxtv.org/master \
		&& git push gitlab kbingham/gitlab-ci -f
	git fetch gitlab

.PHONY:
