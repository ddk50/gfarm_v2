all: man-all post-all-hook
install: man-install post-install-hook
clean: man-clean post-clean-hook
veryclean: man-veryclean post-very-clean-hook
distclean: man-distclean post-distclean-hook
gfregister: man-gfregister post-gfregister-hook
man: man-man
html: man-html
msgno: man-msgno
catalog: man-catalog

post-all-hook:
post-install-hook:
post-clean-hook:
post-very-clean-hook:
post-distclean-hook:
post-gfregister-hook:

man-all:

man-install:
	@for i in -- $(MAN); do \
		case $$i in --) continue;; esac; \
		suffix=`expr $$i : '.*\.\([^.]*\)$$'`; \
		$(MKDIR_P) $(DESTDIR)$(mandir)/man$$suffix; \
		echo \
		$(INSTALL_DOC) $(srcdir)/$$i \
			$(DESTDIR)$(mandir)/man$$suffix/$$i; \
		$(INSTALL_DOC) $(srcdir)/$$i \
			$(DESTDIR)$(mandir)/man$$suffix/$$i; \
	done

man-clean:
	-test -z "$(DOCBOOK2MAN_XSL)" || $(RM) -f $(DOCBOOK2MAN_XSL)
	-test -z "$(EXTRA_CLEAN_TARGETS)" || $(RM) -f $(EXTRA_CLEAN_TARGETS)

man-veryclean: clean
	-test -z "$(EXTRA_VERYCLEAN_TARGETS)" || $(RM) -f $(EXTRA_VERYCLEAN_TARGETS)

man-distclean: veryclean
	-test ! -f $(srcdir)/Makefile.in || $(RM) -f Makefile

man-gfregister:

$(DOCBOOK2MAN_XSL): $(srcdir)/$(DOCBOOK2MAN_XSL).in
	for i in -- $(DOCBOOK_XSLDIRS); do \
		case $$i in --) continue;; esac; \
		test -d $$i \
			&& sed -e "s|@DOCBOOK_XSLDIR@|$$i|" $? > $@ \
			&& exit 0; \
	done; \
	echo "No DocBook XSL directory found."; \
	exit 1

$(dstsubst): $(srcsubst)
	$(XSLTPROC) $(DOCBOOK2MAN_XSL) $(srcsubst) > $(dstsubst)

man-man: $(DOCBOOK2MAN_XSL)
	for i in $(DOCBOOK); do \
		$(MAKE) -f $(srcdir)/Makefile \
			srcsubst=$(DOCBOOK_DIR)/$${i}.docbook \
			dstsubst=$$i $$i; \
	done

man-html:
man-msgno:
man-catalog:
