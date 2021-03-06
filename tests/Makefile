#
# Copyright 2017 GoPro Inc.
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

BACKEND ?= auto
HAS_COMPUTE := $(or $(shell ngl-probe -l quiet -b $(BACKEND) --cap compute | tail -n1),0)
MAX_SAMPLES := $(or $(shell ngl-probe -l quiet -b $(BACKEND) --cap max_samples | tail -n1),0)

ifeq ($(V),)
Q   := @
OUT := > /dev/null
else
Q   :=
OUT :=
endif

all: tests

# 1:category, 2:name
define DECLARE_TEST
test-$(1)-$(2): FUNC_NAME = $(1)_$(2)
TESTS_$(1) += test-$(1)-$(2)
endef

# 1:destvar 2:category, 3:namelist
define DECLARE_TESTS
$(foreach X,$(3),$(eval $(call DECLARE_TEST,$(2),$(X))))
test-$(2): $$(TESTS_$(2))
.PHONY: test-$(2)
$(1) += $$(TESTS_$(2))
$$(TESTS_$(2)): MODULE = $(2)
endef

# 1:category, 2:namelist
define DECLARE_SIMPLE_TESTS
$(call DECLARE_TESTS,SIMPLE_TESTS,$(1),$(2))
endef

# 1:category, 2:namelist
define DECLARE_REF_TESTS
$(call DECLARE_TESTS,REF_TESTS,$(1),$(2))
endef

include anim.mak
include api.mak
include blending.mak
include compute.mak
include data.mak
include live.mak
include media.mak
include rtt.mak
include shape.mak
include text.mak
include texture.mak
include transform.mak

$(REF_TESTS):
	@echo $@
	$(Q)ngl-test $(MODULE).py $(FUNC_NAME) refs/$(FUNC_NAME).ref $(OUT)

$(SIMPLE_TESTS):
	@echo $@
	$(Q)ngl-test $(MODULE).py $(FUNC_NAME) $(OUT)

TESTS = $(SIMPLE_TESTS) $(REF_TESTS)

tests: $(TESTS)

$(TESTS): tests-info
tests-info:
	$(Q)ngl-probe -l info -b $(BACKEND)

tests-list:
	@for test in $(TESTS); do echo $$test; done

.PHONY: all tests tests-info tests-list $(TESTS)
