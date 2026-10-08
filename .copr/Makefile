# COPR "make srpm" build method: COPR runs `make -f .copr/Makefile srpm
# outdir=<dir> spec=<spec>` in a fresh checkout.
srpm:
	dnf -y install git tar rpm-build
	$(MAKE) srpm DIST=$(outdir)
