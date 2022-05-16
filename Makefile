
CONF = CONFIG_ASFS_RW=y CONFIG_ASFS_DEFAULT_CODEPAGE=none

default:
	cd src && $(MAKE) $@ CONFIG_ASFS_FS=m $(CONF) && cp asfs.ko .. && modinfo asfs.ko || break ;

clean:
	cd src && $(MAKE) $@ CONFIG_ASFS_FS=m $(CONF) && rm -f ../asfs.ko || break ;
