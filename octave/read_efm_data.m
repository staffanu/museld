function [filtered, taps]=read_efm_data(size, cutoffMHz, order, filter_decim)

fid = fopen('/data/captures/jp-muse-rf-62.5MHz-nofilter.raw','r');
unfiltered = fread(fid, [1 size], 'int16');
fclose(fid);

Fs=62.5e6;

taps=fir1(order, cutoffMHz * 1e6 / (Fs/2), 'low');
convoluted=conv(unfiltered, taps, 'valid');

filtered=convoluted(1:filter_decim:end);

endfunction

