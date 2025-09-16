function [ac3,carrier] = read_ac3()

size=1000000;
fid = fopen('/home/staffanu/work/ldaudio/data/mars-62.5MHz-filtered.bin','r');
unfiltered = fread(fid, [1 size], 'uint8');
fclose(fid);

unfiltered=clip((unfiltered/128-1) * 10, [-1, 1]);

Fs=62.5e6;
Fc=2.88e6;
Fd=150e3;

Fo=46.08e6;

#assert(Fs/Fo==1625/1152);

taps=fir1(15, [Fc-Fd, Fc+Fd]/(Fs/2), 'bandpass');
filtered=conv(unfiltered, taps, 'valid');

ac3=resample(filtered, 4608, 6250);

Fm=10e3;
carrier=conv(ac3.^4, fir1(31, [4*Fc-Fm, 4*Fc+Fm]/(Fo/2), 'bandpass'));

endfunction
