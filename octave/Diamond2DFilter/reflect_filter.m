% Takes the upper left quarter of a filter response and mirrors
% the coefficients so that a vertially and horizontally symmtric filter
% is obtained.  If the input is an n x m matrix, the result is (2*n-1)x(2*m-1),
% that is, the filter has an odd symmetry.
function F=reflect_filter(M)
  sx=size(M)(2);
  sy=size(M)(1);
  F=[M M(:, sx-1:-1:1);
     M(sy-1:-1:1, :) M(sy-1:-1:1, sx-1:-1:1)];
endfunction

