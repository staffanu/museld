pkg load signal
pkg load optim

% 16 tap lowpass filter.  This will not eliminate the pilot signal
% very well so that has to be taken care of by the efm equalization filter.
% The import thing is that everything below F_s / 8 is eliminated for decimation.
[data, lp]=read_efm_data(20000000, 2, 15, 4);

% Create an EFM filter from guessing what the frequency /phase response should
% be.  Inspiration from the lddecode project.
% We evaluate it by checking how well it separates symbols in the input.
coeffs=make_efm_filter(1024, 4);
eval_efm_fir_filter(coeffs, data, 4, true)

% Take the center taps to get an approximation -- this will not be very good.
% Here we pick the center 32 taps
c=coeffs(498:529); plot(c); length(c)
eval_efm_fir_filter(c, data, 4, true) % looks ok

% See if we can optimize the filters to get better symbol separation

target=@(x) eval_efm_fir_filter(x', data, 4, false); % true to see progress

[p, objf, cvg, outp] = nonlin_min(target, c',
   optimset('algorithm', 'samin',
            'lbound', -ones(length(c), 1),
            'ubound', ones(length(c),1),
            'Display', 'iter'));

eval_efm_fir_filter(p', data, 4, true) % Clear improvment -- better than coeffs!

printArrayAsCode(lp);
printArrayAsCode(p);

