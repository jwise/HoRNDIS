# For the building the driver and Package for macOS12, macOS11
# XCode version 13.2.
#

HoRNDIS:
	@echo "Building HoRNDIS..."
	@echo [XCODE] $(PROGRAMS)
	@echo "Building Package"
	@$(CURDIR)/Build-Package
	
