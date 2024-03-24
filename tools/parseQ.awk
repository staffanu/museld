#!/usr/bin/awk -f

BEGIN {
    for(i=0; i<16; i++) {
	H[sprintf("%x",i)]=i; H[sprintf("%X",i)]=i
    }
}

/RS1C/ { rs1c += parsehex($2$3$4$5) }
/RS2C/ { rs2c += parsehex($2$3$4$5) }
/RS F/ { rsf += parsehex($3$4$5$6) }
/TOT/  { tot += parsehex($2$3$4$5); totmissing += 1850 - parsehex($2$3$4$5); }

END {
    print "RS1C", rs1c
    print "RS2C", rs2c
    print "RS F", rsf
    print "TOT ", tot
    print "TOT missing", totmissing
}

function parsehex(v,out) {
    for (i=1; i <= length(v); i++)
	out=(out*16) + H[substr(v, i, 1)];
    return(out);
}
