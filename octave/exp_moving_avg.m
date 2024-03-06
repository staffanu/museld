function avg = exp_moving_avg(x, alpha)
  avg = filter([alpha], [1 alpha-1], x, sum(x)/length(x));
end

