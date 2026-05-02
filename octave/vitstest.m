1;

function data=readdata(offset, count)
  fid = fopen('/data/captures/sony-the-test-disc.muse','r');
  fseek(fid, offset);
  data = fread(fid, [1 count], 'int16');
  fclose(fid);
endfunction

sk=2*480*1125*30*218 + 2*480*563;

line1=readdata(sk, 480);

deemph = [ 1/32 1/16 5/32 1/2 5/32 1/16 1/32 ];

line1_deemph=conv(line1, deemph);



