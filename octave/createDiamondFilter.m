function filter = createDiamondFilter(nx, ny, xFactor, yFactor)

%[X, Y] = meshgrid([-1:2 / (nx-1):1], [-1:2/ (ny-1):1]);
xfreqs = [ -(ceil((nx-1)/2):-1:1), 0, (1:floor((nx-1)/2)) ] / nx;
yfreqs = [ -(ceil((ny-1)/2):-1:1), 0, (1:floor((ny-1)/2)) ] / ny;
[X, Y] = meshgrid(xfreqs, yfreqs);

response = @(x) (x < 0.9) * 1.0 + (x >= 0.9 & x < 1.1) .* (0.5 + 1 - x);

diamond = response(abs(X * xFactor) + abs(Y * yFactor))

filter = make_filter(ifftshift(diamond), true, true);

endfunction

