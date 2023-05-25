MAKEFILES	:=	sysmod overlay
TARGETS		:= $(foreach dir,$(MAKEFILES),$(CURDIR)/$(dir))

all: $(TARGETS)
	@mkdir -p out/
	@cp -R sysmod/out/* out/
	@cp -R overlay/out/* out/

.PHONY: $(TARGETS)

$(TARGETS):
	@$(MAKE) -C $@

clean:
	@rm -rf out
	@for i in $(TARGETS); do $(MAKE) -C $$i clean || exit 1; done;

dist: all
	@for i in $(TARGETS); do $(MAKE) -C $$i dist || exit 1; done;
	@echo making dist ...

	@rm -f sys-patch.zip
	@cd out; zip -r ../sys-patch.zip ./*; cd ../
