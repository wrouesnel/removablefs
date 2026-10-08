# Packaging helpers.  The kernel module itself lives in module/ (see its
# Makefile); this file builds release tarballs and packages from the tree.
VERSION := $(shell sed -n 's/^Version: *//p' rpm/ownfs.spec)
DIST    ?= dist
ABSDIST := $(abspath $(DIST))

all: tarball

# From a git checkout: the files git knows or would add (not ignored ones),
# as they are in the tree.  From an unpacked tarball: everything but dist/.
tarball:
	mkdir -p $(DIST)
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
		git ls-files -z --cached --others --exclude-standard | tar --null -T - \
			--transform 's,^,ownfs-$(VERSION)/,' --owner=0 --group=0 \
			-czf $(DIST)/ownfs-$(VERSION).tar.gz; \
	else \
		tar --exclude='./$(DIST)' --transform 's,^\.,ownfs-$(VERSION),' \
			--owner=0 --group=0 -czf $(DIST)/ownfs-$(VERSION).tar.gz .; \
	fi

srpm: tarball
	rpmbuild -bs --define "_sourcedir $(ABSDIST)" --define "_srcrpmdir $(ABSDIST)" rpm/ownfs.spec

rpm: tarball
	rpmbuild -ba --define "_topdir $(ABSDIST)/rpmbuild" --define "_sourcedir $(ABSDIST)" \
		--define "_rpmdir $(ABSDIST)" --define "_srcrpmdir $(ABSDIST)" rpm/ownfs.spec

# Binary .debs land in $(DIST).
deb:
	mkdir -p $(DIST)
	dpkg-buildpackage -us -uc -b
	mv ../ownfs_$(VERSION)*.deb ../ownfs-dkms_$(VERSION)*.deb ../ownfs_$(VERSION)*.buildinfo ../ownfs_$(VERSION)*.changes $(DIST)/

check-version:
	scripts/check-version.sh

clean:
	rm -rf $(DIST)
	$(MAKE) -C module clean 2>/dev/null || true

.PHONY: all tarball srpm rpm deb check-version clean
