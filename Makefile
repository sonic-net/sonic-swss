.ONESHELL:
SHELL=/bin/bash
.SHELLFLAGS += -e

BAZEL_BUILD_TARGETS = //:swss_deb_pkg //:swss_deb_dbg_pkg

# Build optimized, stripped binaries.
BAZEL_BUILD_OPTS += -c opt

MAIN_TARGET = $(SWSS)
DERIVED_TARGETS = $(SWSS_DBG)

$(addprefix $(DEST)/, $(MAIN_TARGET)): $(DEST)/% :
	function cleanup {
		# Note: make seems to hang if Bazel is still running
		bazel $(BAZEL_OPTS) shutdown
	}
	trap cleanup EXIT
	bazel $(BAZEL_OPTS) build $(BAZEL_BUILD_OPTS) $(BAZEL_BUILD_TARGETS)
	pushd ./bazel-bin/
	mv $* $(DERIVED_TARGETS) $(DEST)/
	popd

$(addprefix $(DEST)/, $(DERIVED_TARGETS)): $(DEST)/% : $(DEST)/$(MAIN_TARGET)