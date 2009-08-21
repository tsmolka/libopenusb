xml_files = manual.xml intro.xml api.xml functions.xml examples.xml

# For when we have a man page :)
man_MANS = 

EXTRA_DIST = manual.xml api.xml examples.xml functions.xml \
	intro.xml website.dsl $(man_MANS)

# I grabbed this same hack from the VACM docs/Makfile.am
CLEANFILES = manual.dvi manual.aux manual.tex manual.log \
	manual.ps.gz; rm -rf html

if BUILD_DOCS
#MANUALS = manual.ps.gz html/index.html
MANUALS = html/index.html html/openusb_guide.html
# Generating postscript takes forever on my laptop apparentely
else
MANUALS =
endif

if SUNOS_API
XMLDCL = /usr/share/sgml/xml.dcl
endif
if DARWIN_API
XMLDCL = /sw/share/sgml/xml.dcl
else
XMLDCL = /usr/share/sgml/openjade/xml.dcl 
endif

all: $(MANUALS)

manual.ps.gz: $(xml_files) website.dsl
	@JADE@ -t ps -d website.dsl\#print $(XMLDCL) manual.xml
	gzip manual.ps

html/index.html: $(xml_files) website.dsl
	rm -rf html
	mkdir html
	@JADE@ -t xml -d website.dsl\#html $(XMLDCL) manual.xml

html/openusb_guide.html: $(xml_files) website.dsl
	test -d html || mkdir html
	@JADE@ -V nochunks -t xml -d website.dsl\#html $(XMLDCL) manual.xml > html/openusb_guide.html

