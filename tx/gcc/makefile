COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR ?= $(abspath ../../../../../..)

include $(COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR)/imports.mak

CC = "$(GCC_ARMCOMPILER)/bin/arm-none-eabi-gcc"
LNK = "$(GCC_ARMCOMPILER)/bin/arm-none-eabi-gcc"

OBJECTS = easylink_config.obj EasyLink_nortos.obj main_nortos.obj smartrf_settings.obj smartrf_settings_predefined.obj rfEasyLinkTx_nortos.obj CC1310_LAUNCHXL.obj CC1310_LAUNCHXL_fxns.obj ccfg.obj

NAME = rfEasyLinkTx

CFLAGS = -I.. \
    -DDeviceFamily_CC13X0 \
    -DCCFG_FORCE_VDDR_HH=0 \
    "-I$(COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR)/source" \
    -DSUPPORT_PHY_CUSTOM \
    -DSUPPORT_PHY_50KBPS2GFSK \
    -DSUPPORT_PHY_625BPSLRM \
    -DSUPPORT_PHY_5KBPSSLLR \
    "-I$(COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR)/kernel/nortos" \
    "-I$(COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR)/kernel/nortos/posix" \
    -mcpu=cortex-m3 \
    -march=armv7-m \
    -mthumb \
    -std=c99 \
    -mfloat-abi=soft \
    -ffunction-sections \
    -fdata-sections \
    -g \
    -gstrict-dwarf \
    -Wall \
    "-I$(GCC_ARMCOMPILER)/arm-none-eabi/include/newlib-nano" \
    "-I$(GCC_ARMCOMPILER)/arm-none-eabi/include"

LFLAGS = -Wl,-T,../gcc/CC1310_LAUNCHXL_NoRTOS.lds \
    "-Wl,-Map,$(NAME).map" \
    "-L$(COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR)/source" \
    -l:ti/display/lib/display.am3g \
    -l:ti/grlib/lib/gcc/m3/grlib.a \
    -l:third_party/spiffs/lib/gcc/m3/spiffs_cc26xx.a \
    -l:ti/drivers/rf/lib/rf_multiMode_cc13x0.am3g \
    -l:ti/drivers/lib/drivers_cc13x0.am3g \
    -eresetISR \
    "-L$(COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR)/kernel/nortos" \
    -l:lib/nortos_cc13x0.am3g \
    -l:ti/devices/cc13x0/driverlib/bin/gcc/driverlib.lib \
    -march=armv7-m \
    -mthumb \
    -nostartfiles \
    -static \
    -Wl,--gc-sections \
    "-L$(GCC_ARMCOMPILER)/arm-none-eabi/lib/thumb/v7-m/nofp" \
    -lgcc \
    -lc \
    -lm \
    -lnosys \
    --specs=nano.specs

all: $(NAME).out

easylink_config.obj: ../easylink/easylink_config.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

EasyLink_nortos.obj: ../easylink/EasyLink_nortos.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

main_nortos.obj: ../main_nortos.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

smartrf_settings.obj: ../smartrf_settings/smartrf_settings.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

smartrf_settings_predefined.obj: ../smartrf_settings/smartrf_settings_predefined.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

rfEasyLinkTx_nortos.obj: ../rfEasyLinkTx_nortos.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

CC1310_LAUNCHXL.obj: ../CC1310_LAUNCHXL.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

CC1310_LAUNCHXL_fxns.obj: ../CC1310_LAUNCHXL_fxns.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

ccfg.obj: ../ccfg.c
	@ echo Building $@
	@ $(CC) $(CFLAGS) $< -c -o $@

$(NAME).out: $(OBJECTS)
	@ echo linking $@
	@ $(LNK)  $(OBJECTS) $(LFLAGS) -o $(NAME).out

clean:
	@ echo Cleaning...
	@ $(RM) $(OBJECTS) > $(DEVNULL) 2>&1
	@ $(RM) $(NAME).out > $(DEVNULL) 2>&1
	@ $(RM) $(NAME).map > $(DEVNULL) 2>&1
