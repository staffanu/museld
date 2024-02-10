function filtered=read_efm_data(size, n_taps, filter_decim)

fid = fopen('/data/captures/jp-muse-rf-62.5MHz-nofilter.raw','r');
unfiltered = fread(fid, [1 size], 'int16');
fclose(fid);

Fs=62.5e6;

lp=fir1(n_taps, 1.7e6 / (Fs/2));
convoluted=conv(unfiltered, lp, 'valid');

filtered=convoluted(1:filter_decim:end);

endfunction

