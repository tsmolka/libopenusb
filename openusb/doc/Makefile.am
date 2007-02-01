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
MANUALS = html/index.html
# Generating postscript takes forever on my laptop apparentely
else
MANUALS =
endif

all: $(MANUALS)

manual.ps.gz: $(xml_files) website.dsl
	@JADE@ -t ps -d website.dsl\#print xml.dcl manual.xml
	gzip manual.ps

html/index.html: $(xml_files) website.dsl
	rm -rf html
	mkdir html
	@JADE@ -t xml -d website.dsl\#html xml.dcl manual.xml

