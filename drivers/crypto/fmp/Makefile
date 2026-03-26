# Exynos FMP makefile
CONFIG_EXYNOS_FMP_FIPS_FUNC_TEST=n

ccflags-$(CONFIG_EXYNOS_FMP_FIPS_FUNC_TEST) += -DCONFIG_EXYNOS_FMP_FIPS_FUNC_TEST

obj-$(CONFIG_EXYNOS_FMP)   += fmp-core.o
fmp-core-$(CONFIG_EXYNOS_FMP) += $(FIPS_PR)fmp.o
fmp-core-$(CONFIG_EXYNOS_FMP_FIPS) += $(FIPS_PR)fmp_test.o \
				$(FIPS_PR)fmp_fips_main.o \
				$(FIPS_PR)fmp_fips_fops.o \
				$(FIPS_PR)fmp_fips_selftest.o \
				$(FIPS_PR)fmp_fips_integrity.o \
				$(FIPS_PR)hmac-sha256.o \
				$(FIPS_PR)fmp_fips_cipher.o \
				$(FIPS_PR)sha256.o

fmp-core-$(CONFIG_EXYNOS_FMP_FIPS_FUNC_TEST) += fmp_fips_func_test.o