#!/bin/sh
    # [environment variables can be set here]

filename=$1


R --slave <<EOF
	"ad.test" <-
	function (x) 
	{
	    DNAME <- deparse(substitute(x))
	    x <- sort(x[complete.cases(x)])
	    n <- length(x)
	    if (n < 8) 
		stop("sample size must be greater than 7")
	    p <- pnorm((x - mean(x))/sd(x))
	    h <- (2 * seq(1:n) - 1) * (log(p) + log(1 - rev(p)))
	    A <- -n - mean(h)
	    AA <- (1 + 0.75/n + 2.25/n^2) * A
	    if (AA < 0.2) {
		pval <- 1 - exp(-13.436 + 101.14 * AA - 223.73 * AA^2)
	    }
	    else if (AA < 0.34) {
		pval <- 1 - exp(-8.318 + 42.796 * AA - 59.938 * AA^2)
	    }
	    else if (AA < 0.6) {
		pval <- exp(0.9177 - 4.279 * AA - 1.38 * AA^2)
	    }
	    else {
		pval <- exp(1.2937 - 5.709 * AA + 0.0186 * AA^2)
	    }
	    RVAL <- list(statistic = c(A = A), p.value = pval, method = "Anderson-Darling normality test", 
		data.name = DNAME)
	    class(RVAL) <- "htest"
	    return(RVAL)
	}

	arquivo <- read.table("$filename", sep=" ", header=TRUE)
	saida =	ad.test(arquivo$"V");
	print(saida)
EOF
	


