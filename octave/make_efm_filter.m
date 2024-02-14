% The following constructs a quite small filter for efm,
% using a lowpass filter of only 15 taps, followed by decimation by 4
% and then a 16 tap efm filter:
%
% data = read_efm_data(10000000, 15, 4);
% coeffs=make_efm_filter(256, 4);
% c=coeffs(122:137); plot(c);
% eval_efm_fir_filter(c, data, 4, true)
%
% L is the size of the ifft
# decimation tells how much the input was decimated from 62.5 MHz
function coeffs = make_efm_filter(L, decimation)

set_amplitudes=[0  0.50  0.80  1.00  1.00  1.00  1.00  0.67  0.33    0];
set_phases=    [0     1     1     1     1     1     1     1     1    1] * 1.20;
Fs=62.5e6 / decimation;
Ts=1/Fs;

% wanted amplitude response for the frequencies in f
function a=amplitude_response(f, ref_amps)
  ref_freqs=[(0:9)*200e3];
  f1 = f(f<=1.6e6);
  fi=interp1(ref_freqs, ref_amps, f1, 'cubic');
  a = max(0, [fi zeros(1, length(f)-length(f1)*2+1) flip(fi(2:end))]);
endfunction

function a=phase_response(f, ref_phases)
  ref_freqs=[(0:9)*200e3];
  f1 = f(f<=1.6e6);
  phi=interp1(ref_freqs, ref_phases, f1, 'cubic');
  a = [phi zeros(1, length(f)-length(f1)*2+1) -flip(phi(2:end))];
endfunction

freqs = (0:L-1) .* (Fs/L);
ar = amplitude_response(freqs, set_amplitudes);
pr = phase_response(freqs, set_phases);

response = ar .* exp(i * pr);

coeffs = real(fftshift(ifft(response)));

endfunction;

