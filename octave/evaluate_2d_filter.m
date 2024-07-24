% The filter F is a matrix with filter coefficients.
% The filter is padded to an n x n matrix before evaluation, so n
% determines the grid size of the FFT.
% lb and ub are the lower and upper bounds of the desired frequency
% response.  They are both functions of two variables (x, y) that
% represent the frequency in radians.
% If F has odd size (both horizonally and vertically), n should be odd.
% If F has even size, n should be even.
% F needs to be either even size both horizontally and vertically, or even
% in both directions.
function e = evaluate_2d_filter(F, n, lb, ub, show=false)

pad_x = (n - size(F)(2)) / 2;
pad_y = (n - size(F)(1)) / 2;

F2 = [ zeros(pad_y, n);
       zeros(size(F)(1), pad_x) F zeros(size(F)(1), pad_x);
       zeros(pad_y, n) ];

x=y=linspace(-pi + pi / n, pi - pi/n, n);
[xx, yy]=meshgrid(x, y);

LB=arrayfun(lb, xx, yy);
UB=arrayfun(ub, xx, yy);

R=fftshift(fft2(ifftshift(F2)));
if max(max(imag(R))) > 1e-8
  error "Too large imaginary part in filter"
endif
R=real(R);

e = sum(sum(((R > UB) .* (R - UB)).^2 + ((R < LB) .* (LB - R)).^2));

if show == 1
  mesh(x, y, R);
elseif show == 2
  contour(x, y, R);
endif

endfunction
