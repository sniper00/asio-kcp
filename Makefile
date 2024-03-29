# GNU Make workspace makefile autogenerated by Premake

ifndef config
  config=debug
endif

ifndef verbose
  SILENT = @
endif

ifeq ($(config),debug)
  kcp_config = debug
  tcp_over_kcp_config = debug
endif
ifeq ($(config),release)
  kcp_config = release
  tcp_over_kcp_config = release
endif

PROJECTS := kcp tcp_over_kcp

.PHONY: all clean help $(PROJECTS) 

all: $(PROJECTS)

kcp:
ifneq (,$(kcp_config))
	@echo "==== Building kcp ($(kcp_config)) ===="
	@${MAKE} --no-print-directory -C projects/build/kcp -f Makefile config=$(kcp_config)
endif

tcp_over_kcp: kcp
ifneq (,$(tcp_over_kcp_config))
	@echo "==== Building tcp_over_kcp ($(tcp_over_kcp_config)) ===="
	@${MAKE} --no-print-directory -C projects/build/tcp_over_kcp -f Makefile config=$(tcp_over_kcp_config)
endif

clean:
	@${MAKE} --no-print-directory -C projects/build/kcp -f Makefile clean
	@${MAKE} --no-print-directory -C projects/build/tcp_over_kcp -f Makefile clean

help:
	@echo "Usage: make [config=name] [target]"
	@echo ""
	@echo "CONFIGURATIONS:"
	@echo "  debug"
	@echo "  release"
	@echo ""
	@echo "TARGETS:"
	@echo "   all (default)"
	@echo "   clean"
	@echo "   kcp"
	@echo "   tcp_over_kcp"
	@echo ""
	@echo "For more information, see https://github.com/premake/premake-core/wiki"