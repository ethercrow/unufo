CXX = g++

GIMPTOOL = gimptool-2.0

GIMP_LDFLAGS=`$(GIMPTOOL) --libs`
GIMP_CFLAGS=`$(GIMPTOOL) --cflags`

CXXFLAGS=$(GIMP_CFLAGS) -O2 -fno-common -ffast-math -frename-registers -fomit-frame-pointer -Wall -Wextra -pedantic -std=c++0x

LDFLAGS=$(GIMP_LDFLAGS) -lm -lboost_thread


all: resynth
	@echo
	@echo 'Now type "make install" to install resynthesizer'
	@echo 

install: resynth smart-remove.scm
	$(GIMPTOOL) --install-bin resynth
	$(GIMPTOOL) --install-script smart-remove.scm
	@echo
	@echo After restarting the Gimp you should find the
	@echo following items in the pop-up image menu:
	@echo
	@echo "  * Filters/Enhance/Heal selection"
	@echo

resynth: resynth.cc *.h
	$(CXX) $(CXXFLAGS) -o $@ resynth.cc $(LDFLAGS)

clean:
	-rm -f *~ *.o core resynth

