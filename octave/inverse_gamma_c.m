function x=inverse_gamma_c(v)
  a = (-48.0/11.0);
  b = (67.0/33.0);
  c = (-1.0/132.0);

  sgn = sign(v - 128.0);
  v_magnitude = abs(v - 128.0);
  y = min(112.0, v_magnitude) / 112.0;
  if y < 5.0 / 72.0
    x = 0.6 * y;
  elseif y < 47 / 33 / 8
    x = -b / (2.0 * a) - sqrt(b * b / (4.0 * a * a) - (c - y) / a);
  else
    x = 33 / 31 * (y - 2 / 33);
  endif
  x = 128.0 + x * sgn * 112.0;
endfunction

