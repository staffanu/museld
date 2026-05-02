% reads 1125*480 samples offset by 1125*480*`offset_frames` samples
function x=read_frame(offset_frames)

fid = fopen('/Users/staffanu/work/ldaudio/data/muse/small.muse','r');
fseek(fid, 1125*480*offset_frames*2);
x = fread(fid, [1 1125*480], 'uint16') / 4;
fclose(fid);

endfunction

