#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory for object files & intermediate files
# SOURCES is a list of directories containing code
# INCLUDES is a list of directories containing extra header files
# DATA is a list of directories containing binary data
#---------------------------------------------------------------------------------
TARGET		:=	sound3ds
BUILD		:=	build
SOURCES		:=	source source/helix-aac
DATA		:=	data
INCLUDES	:=	include source/helix-aac source/helix-aac/compat
ROMFS		:=	romfs

APP_TITLE	:=	Sound3DS
APP_DESCRIPTION	:=	Reproductor Estetico SoundCloud
APP_AUTHOR	:=	Peppi & Antigravity

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -DARM11 -DUSE_DEFAULT_STDLIB
# Banderas extra desde la línea de comandos (por ejemplo, la compilación de prueba automática)
CFLAGS	+=	$(EXTRA)

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

PORTLIBS	:= $(DEVKITPRO)/portlibs/3ds
LIBDIRS		:= $(PORTLIBS) $(CTRULIB)

LIBS	:= -lcitro2d -lcitro3d -ljansson -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN		:=	$(addsuffix .o,$(BINFILES))
export OFILES			:=	$(OFILES_BIN) $(OFILES_SOURCES)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(OUTPUT).smdh
export _3DSXFLAGS	:=	--smdh=$(CURDIR)/$(TARGET).smdh

ifneq ($(strip $(ROMFS)),)
	ifneq ($(wildcard $(CURDIR)/$(ROMFS)),)
		export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
	endif
endif

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo Limpiando build...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).map

#---------------------------------------------------------------------------------
else

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OUTPUT).elf	:	$(OFILES)

$(OUTPUT).smdh	:
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $(OUTPUT).smdh

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

# Helix AAC es código de terceros: se silencia su único aviso en vez de modificarlo
pns.o: CFLAGS += -Wno-unused-but-set-variable

-include $(DEPSDIR)/*.d

endif
