#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SRC_DIR := $(PWD)

# KOO TODO: Add the soruces here
C_SRCS := $(SRC_DIR)/rping.c $(SRC_DIR)/common.c
#C_SRCS := $(SRC_DIR)/rping_test.c $(SRC_DIR)/common.c
# KOO TODO: Add include files here
INCLUDES := -I$(SRC_DIR)/include -Ilib/rdma-core/build/include

CFLAGS += $(INCLUDES)
OBJS := $(C_SRCS:%.c=%.o)

#APP := $(SPDK_ROOT_DIR)/build/$(APP)$(EXEEXT)
APP = rdma_module

LIBS += -libverbs -lrdmacm -lpthread

CLEAN_FILES = $(APP)

all : $(APP)
	@:

install: empty_rule

uninstall: empty_rule

# To avoid overwriting warning
empty_rule:
	@:

$(APP) : $(OBJS) $(ENV_LIBS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

clean :
	rm -rf $(OBJS) $(APP)
