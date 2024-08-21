% Implements (almost) one iteration of the iterative algorithm from
% Chapter 91.3: Two-Dimensional FIR Filters by Rashid Ansari and A. Enis Cetin,
% from The Circuits and Filters Handbook (eBook ISBN 9780429128790).

function Fnew=iterate_filter(F)
  global n x y LB UB; % n is the grid size for LB and UB

  if size(LB)(1) != n || size(LB)(2) != n || size(UB)(1) != n || size(UB)(2) != n
    error "Incorrect LB/UB dimension"
  endif

  pad_x = (n - size(F)(2)) / 2;
  pad_y = (n - size(F)(1)) / 2;

  % padded input filter
  Fp=[ zeros(pad_y, n);
       zeros(size(F)(1), pad_x) F zeros(size(F)(1), pad_x);
       zeros(pad_y, n) ];

  R=fftshift(fft2(ifftshift(Fp)));
  if max(max(imag(R))) > 1e-8
    error "Too large imaginary part in filter"
  endif
  R=real(R);

  R2=min(UB, max(LB, R)); % frequency response limited by LB and UB

  F2=fftshift(ifft2(ifftshift(Fp))); % back to sample domain

  Fnew=F2(pad_y+1:pad_y+size(F)(1), pad_x+1:pad_x+size(F)(2));
  Fnew=Fnew/sum(sum(Fnew));

  mesh(F2)
  pause
  mesh(Fnew)
endfunction

