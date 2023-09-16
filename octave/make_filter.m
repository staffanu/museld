% M is the desired the frequency response of the filter, with the zero
% frequency at (1,1).  The dimensions of the filter will be the same as M.

function filter = make_filter(M, do_window, do_normalize)

cfilter = fftshift(ifft2(M));

if do_window
  rolloff = hamming(size(M, 1)) * hamming(size(M, 2))';
  rolledoff = cfilter .* rolloff;
else
  rolledoff = cfilter;
endif

if do_normalize
  filter = rolledoff / sum(sum(rolledoff));
else
  filter = rolledoff;
endif

if max(max(imag(filter))) > 1e-10
  error "Too large imaginary parts of filter coefficients"
endif

filter=real(filter);

endfunction

% To check the response:
% F=make_filter(...)
% R=fftshift(abs(fft2(F)));
% [x, y] = meshgrid(1:size(F, 2), 1:size(F, 1));
% surf(x, y, R);

