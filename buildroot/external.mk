# Buildroot includes this file from every external tree.
# Pull in the .mk file of each of our own packages.
include $(sort $(wildcard $(BR2_EXTERNAL_PERKINSOS_PATH)/package/*/*.mk))
