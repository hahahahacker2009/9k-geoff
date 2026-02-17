#boot cpu
#	int cpuflag = 1;
#boot cpu boot $3
#	int cpuflag = 1;
#	char* bootdisk = "$3";
#boot rootdir $3
#	char* rootdir = "$3";
#boot (bboot|romboot|dosboot)
#	int cpuflag = 1;
#	char* bootprog = $2;
#boot boot $3
#	char* bootdisk = "$3";
