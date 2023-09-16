% The resulting filter will have dimensions nx * xy.
% xFactor determines the x axis cutoff frequency: the cutoff frequency
% is the sampling frequency / xFactor.  So, e.g., xFactor=4 makes a lowpass
% filter with a cutoff frequency at half the Nyquist frequency.
% yFactor is treated analogously.

function filter = make_rectangular_lowpass(nx, ny, xFactor, yFactor, do_window)

xfreqs = [ -(ceil((nx-1)/2):-1:1), 0, (1:floor((nx-1)/2)) ] / nx;
yfreqs = [ -(ceil((ny-1)/2):-1:1), 0, (1:floor((ny-1)/2)) ] / ny;
[X, Y] = meshgrid(xfreqs, yfreqs);

response = @(x) (x < 0.9) * 1.0 + (x >= 0.9 & x < 1.1) .* (0.5 + 5 - x * 5);

wanted_freq_response = response(max(abs(X * xFactor), abs(Y * yFactor)))

filter = make_filter(ifftshift(wanted_freq_response), do_window, true);

