CC = g++

GIMPTOOL = gimptool-2.0

GIMP_LDFLAGS=`$(GIMPTOOL) --libs`
GIMP_CFLAGS=`$(GIMPTOOL) --cflags`

CFLAGS=$(GIMP_CFLAGS) -O3 -fno-common -ffast-math -frename-registers -fomit-frame-pointer

LDFLAGS=$(GIMP_LDFLAGS) -lm


all: resynth
	@echo
	@echo 'Now type "make install" to install resynthesizer'
	@echo 

install: resynth smart-enlarge.scm smart-remove.scm
	$(GIMPTOOL) --install-bin resynth
	$(GIMPTOOL) --install-script smart-enlarge.scm
	$(GIMPTOOL) --install-script smart-remove.scm
	@echo
	@echo After restarting the Gimp you should find the
	@echo following items in the pop-up image menu:
	@echo
	@echo "  * Filters/Map/Resynthesize"
	@echo "  * Filters/Enhance/Smart enlarge"
	@echo "  * Filters/Enhance/Smart sharpen"
	@echo "  * Filters/Enhance/Smart remove selection"
	@echo

resynth: resynth.cc
	$(CC) $(CFLAGS) -o $@ resynth.cc $(LDFLAGS)

clean:
	-rm -f *~ *.o core resynth

