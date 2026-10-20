################################################################################
#
# netsurf-lkx: NetSurf as a LiteKern X app (LiteKernX-Roadmap/06, phase L6)
#
# Buildroot's own netsurf package (package/netsurf), with its patches, but:
#   - the framebuffer front end without SDL: libnsfb gets a surface of ours,
#     "lkx" (platform/nsfb_lkx.c, copied in), drawing into a LiteKern X
#     window and taking its input, through the apps' library (libk86)
#   - fonts through FreeType; HTTPS through curl and OpenSSL
#   - no JavaScript yet (phase L7 decides)
#
################################################################################

NETSURF_LKX_VERSION = 3.10
NETSURF_LKX_SOURCE = netsurf-all-$(NETSURF_LKX_VERSION).tar.gz
NETSURF_LKX_SITE = http://download.netsurf-browser.org/netsurf/releases/source-full
NETSURF_LKX_LICENSE = GPL-2.0
NETSURF_LKX_LICENSE_FILES = netsurf/COPYING
NETSURF_LKX_DEPENDENCIES = expat jpeg libpng freetype libcurl openssl \
	host-bison host-flex host-gperf host-pkgconf host-vim host-libpng

# The repository (for platform/nsfb_lkx.c and the headers it includes).
NETSURF_LKX_REPO = $(BR2_EXTERNAL_LITEKERNX_PATH)/..

define NETSURF_LKX_ADD_SURFACE
	cp $(NETSURF_LKX_REPO)/platform/nsfb_lkx.c $(@D)/libnsfb/src/surface/lkx.c
	$(SED) 's/^DIR_SOURCES :=/SURFACE_HANDLER_yes += lkx.c\nDIR_SOURCES :=/' $(@D)/libnsfb/src/surface/Makefile
	echo 'CFLAGS += -I$(NETSURF_LKX_REPO) -DLKX_LINUX -D_FILE_OFFSET_BITS=64' >> $(@D)/libnsfb/Makefile
endef
NETSURF_LKX_POST_EXTRACT_HOOKS += NETSURF_LKX_ADD_SURFACE

# LiteKern X's look for the front end's own furniture: the header grey
# (#2e2e32) for the toolbar and status bar, the button grey (#444448) for the
# scroll bars, a dark address bar with white text, and white line icons
# (make-icons.py). libnsfb's colours are 0xAABBGGRR.
define NETSURF_LKX_RESTYLE
	cp $(NETSURF_LKX_PKGDIR)/icons/*.png $(@D)/netsurf/frontends/framebuffer/res/icons/
	$(SED) 's/^#define FB_FRAME_COLOUR .*/#define FB_FRAME_COLOUR 0xFF322E2E/; s/^#define FB_SCROLL_COLOUR .*/#define FB_SCROLL_COLOUR 0xFF484444/' \
		$(@D)/netsurf/frontends/framebuffer/fbtk.h
	$(SED) 's/FB_COLOUR_WHITE/0xFF221E1E/g; s/FB_COLOUR_BLACK/0xFFFFFFFF/g' \
		$(@D)/netsurf/frontends/framebuffer/gui.c
endef
NETSURF_LKX_POST_EXTRACT_HOOKS += NETSURF_LKX_RESTYLE

define NETSURF_LKX_CONFIGURE_CMDS
	echo "override NETSURF_USE_DUKTAPE := NO"       >> $(@D)/netsurf/Makefile.config
	echo "override NETSURF_FB_FONTLIB := freetype"  >> $(@D)/netsurf/Makefile.config
	echo "override NETSURF_USE_WEBP := NO"          >> $(@D)/netsurf/Makefile.config
	echo "override NETSURF_USE_VIDEO := NO"         >> $(@D)/netsurf/Makefile.config
endef

NETSURF_LKX_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	CFLAGS="$(TARGET_CFLAGS) -I$(@D)/tmpusr/include" \
	LDFLAGS="$(TARGET_LDFLAGS) -L$(@D)/tmpusr/lib"

NETSURF_LKX_MAKE_OPTS = \
	TARGET=framebuffer \
	BISON="$(HOST_DIR)/bin/bison" \
	FLEX="$(HOST_DIR)/bin/flex" \
	PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
	BUILD_CC="$(HOSTCC)" \
	CC="$(TARGET_CC)" \
	AR="$(TARGET_AR)" \
	TMP_PREFIX=$(@D)/tmpusr \
	NETSURF_CONFIG="BUILD_CFLAGS='$(HOST_CFLAGS)' BUILD_LDFLAGS='$(HOST_LDFLAGS) -lpng'" \
	PREFIX=/usr

define NETSURF_LKX_BUILD_CMDS
	mkdir -p $(@D)/tmpusr
	$(NETSURF_LKX_MAKE_ENV) $(MAKE) -C $(@D) $(NETSURF_LKX_MAKE_OPTS) build
endef

define NETSURF_LKX_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(NETSURF_LKX_MAKE_OPTS) \
		DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
