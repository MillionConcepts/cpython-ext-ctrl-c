
## unified linear model for "time per sample", with implementation and GIL
## held/released as categorical predictors; we also throw in an "is
## the size an odd power of two" predictor because there's visible
## oscillation in naive size~time plots.

model0 <- lm(log(elapsed) ~ log(size) + size.oddp2
           + impl + impl:gil + impl:interval,
           data = rt)

model1 <- lm(log(elapsed) ~ log(size)
            + size.oddp2
            + impl + impl:log(size)
            + impl:gil + impl:gil:log(size)
            + impl:interval + impl:interval:log(size),
            data = rt)

model <- lm(elapsed/size ~ size.oddp2 + impl + impl:gil + impl:interval,
             data=rt)

pred <- subset(
    expand.grid(
        impl=levels(rt$impl),
        gil=levels(rt$gil),
        interval=unique(rt$interval),
        size.oddp2=FALSE
    ),
    ifelse(impl %in% c("coarse", "fine"),
           interval == 1,
           interval == 0)
)
pred <- cbind(pred, predict(model, pred, interval="confidence") * 1e6)

ggplot(pred, aes(y=reorder(interaction(impl, gil), fit),
                 x=fit, xmin=lwr, xmax=upr)) +
    geom_pointrange() +
    scale_x_continuous("Nanoseconds per sample",
                       limits=c(0, 140),
                       expand=c(0, 0),
                       breaks=seq(0, 140, by=20))



## try to calculate and factor out the effect of size on time required,
## leaving only the effect of implementation.
## using `ordered(log(size))` instead of just `log(size)` causes lm()
## to fit a 7th-degree polynomial instead of a straight line, which
## would normally be _egregious_ overfitting but in this case it's
## just what we need to eliminate the oscillations due to odd-power-
## of-two sample sizes going through a different code path than even-
## power-of-two sample sizes.

## the result of this calculation is only meaningful in relative terms.

baseline <- lm(log(elapsed) ~ ordered(log(size)), data=rt)
rt$impl.effect <- exp(resid(baseline))
rt$rel.impl.effect <- rt$impl.effect /
    min(subset(rt, impl == "none" & gil == "held")$impl.effect)
