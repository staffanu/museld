pkg load signal
pkg load optim

data=read_efm_data(1000000, 15, 4);
coeffs=make_efm_filter(1024,4);
c=coeffs(498:529); plot(c); length(c)

eval_efm_fir_filter(coeffs, data, 4, true)
eval_efm_fir_filter(c, data, 4, true)

target=@(x) eval_efm_fir_filter(x', data, 4, true);

[p, objf, cvg, outp] = nonlin_min(target, c',
   optimset('algorithm', 'samin',
            'lbound', -ones(length(c), 1),
            'ubound', ones(length(c),1),
            'Display', 'iter'));

eval_efm_fir_filter(p', data, 4, true)

