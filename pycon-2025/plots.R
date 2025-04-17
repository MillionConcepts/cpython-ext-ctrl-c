suppressPackageStartupMessages({
    library(tidyverse)
    library(cowplot)
})

rt <- read_csv("runtime-all-py312.csv")

## just the cost of check-whenever-possible
ggplot(subset(rt, impl %in% c("none", "simple")),
       aes(x=size, y=elapsed * 1e3, colour=impl, shape=impl)) +
    geom_point(size=3) +
    geom_smooth(method="lm", formula=y~x, se=FALSE) +
    scale_x_continuous(
        "Number of samples",
        transform="log2",
        breaks=unique(rt$size),
        labels=scales::label_log(base=2)
    ) +
    scale_y_continuous(
        "Elapsed time (milliseconds)",
        transform="log10",
        limits=c(3, 1000),
        breaks=scales::breaks_log(6)
    ) +
    scale_colour_manual(
        values=c("simple"="#66c2a5", "none"="#8da0cb"),
        breaks=c("simple", "none"),
        labels=c("Check whenever possible", "No checks")
    ) +
    scale_shape_manual(
        values=c("none"=3, "simple"=4),
        breaks=c("simple", "none"),
        labels=c("Check whenever possible", "No checks")
    ) +
    theme_half_open() +
    theme(legend.position=c(.95, .05),
          legend.justification=c(1, 0),
          legend.title=element_blank())

## add in coarse and fine w/ checks every 1ms
ggplot(subset(rt, interval <= 0.001),
       aes(x=size, y=elapsed * 1e3, colour=impl, shape=impl)) +
    geom_point(size=3) +
    geom_smooth(method="lm", formula=y~x, se=FALSE) +
    scale_x_continuous(
        "Number of samples",
        transform="log2",
        breaks=unique(rt$size),
        labels=scales::label_log(base=2)
    ) +
    scale_y_continuous(
        "Elapsed time (milliseconds)",
        transform="log10",
        limits=c(3, 1000),
        breaks=scales::breaks_log(6)
    ) +
    scale_colour_manual(
        values=c("coarse"="#e78ac3", "fine"="#fc8d62",
                 "simple"="#66c2a5", "none"="#8da0cb"),
        breaks=c("coarse", "fine", "simple", "none"),
        labels=c(
            "Check every 1ms (coarse clock)",
            "Check every 1ms (fine clock)",
            "Check whenever possible",
            "No checks"
        )
    ) +
    scale_shape_manual(
        values=c("coarse"=13, "fine"=9, "simple"=3, "none"=2),
        breaks=c("coarse", "fine", "simple", "none"),
        labels=c(
            "Check every 1ms (coarse clock)",
            "Check every 1ms (fine clock)",
            "Check whenever possible",
            "No checks"
        )
    ) +
    theme_half_open() +
    theme(legend.position=c(.95, .05),
          legend.justification=c(1, 0),
          legend.title=element_blank())

rti <- read_csv("runtime-intervals-py312.csv")
rtN <- rti |> subset(impl=="none")   |> select(size|rep|elapsed)
rtC <- rti |> subset(impl=="coarse") |> select(size|rep|interval|elapsed)
rtR <- (
    inner_join(rtC, rtN, by=c('size','rep'), suffix=c('.coarse', '.none'))
    |> mutate(interval = interval * 1e3,
              elapsed.coarse = elapsed.coarse * 1e3,
              elapsed.none = elapsed.none * 1e3,
              size.f = factor(size))
    |> group_by(size, interval)
    |> mutate(elapsed.coarse=sort(elapsed.coarse),
              elapsed.none=sort(elapsed.none),
              elapsed.ratio=elapsed.coarse/elapsed.none)
    |> ungroup()
)

ggplot(subset(rtR, interval=1)) +
    geom_smooth(aes(x=size, y=elapsed.coarse), colour="#045a8d",
                method="lm", formula=y~x, se=FALSE, linewidth=1) +
    geom_smooth(aes(x=size, y=elapsed.none), colour="#e34a33",
                method="lm", formula=y~x, se=FALSE, linewidth=1) +
    scale_x_continuous(
        "Number of samples",
        transform="log2",
        breaks=unique(rt$size),
        labels=scales::label_log(base=2)
    ) +
    scale_y_continuous(
        "Elapsed time (milliseconds)",
        transform="log10",
        limits=c(3, 1000),
        breaks=scales::breaks_log(6)
    ) +
    theme_half_open()

ggplot(rtR) +
    geom_smooth(aes(x=size, y=elapsed.coarse, colour=interval, group=interval),
                method="lm", formula=y~x, se=FALSE, linewidth=1) +
    geom_smooth(aes(x=size, y=elapsed.none), colour="#e34a33",
                method="lm", formula=y~x, se=FALSE, linewidth=1) +
    scale_x_continuous(
        "Number of samples",
        transform="log2",
        breaks=unique(rt$size),
        labels=scales::label_log(base=2)
    ) +
    scale_y_continuous(
        "Elapsed time (milliseconds)",
        transform="log10",
        limits=c(3, 1000),
        breaks=scales::breaks_log(6)
    ) +
    scale_colour_gradient(
        "Check interval",
        low="#045a8d",
        high="#d0d1e6",
    ) +
    theme_half_open() +
    theme(legend.position=c(.95, .05),
          legend.justification=c(1, 0))

ggplot(rtR, aes(x=interval, y=elapsed.ratio, colour=size.f, shape=size.f)) +
    geom_jitter(width=0.25) +
    scale_colour_brewer(palette="Set2") +
    labs(x="Check interval", y="Elapsed time ratio (checks/no checks)",
         colour="Input size", shape="Input size") +
    theme_half_open() +
    theme(legend.position="top")

lat.delay <- (
    read_csv("latency-by-delay-py312.csv")
    |> subset(interrupted & interval <= 0.035)
    |> mutate(interval = interval * 1e3,
              delay = delay * 1e3,
              latency = latency * 1e3)
)

interval.labels <- with(lat.delay, case_when(
    interval == 1 ~ "Check interval: 1 ms",
    .default = sprintf("%.0f ms", interval)
))


ggplot(lat.delay, aes(x=delay, y=latency)) +
    facet_wrap(~interval, nrow=2,
               labeller = as_labeller(
                   function(s) ifelse(s == "1",
                                      "Check interval: 1 ms",
                                      paste(s, "ms")))) +
    geom_point() +
    geom_hline(aes(yintercept=interval), colour="#08519c") +
    labs(x="Time before control-C (ms)",
         y="Delay returning to interpreter (ms)") +
    theme_minimal_grid(colour="white") +
    theme(
        panel.background=element_rect(fill="#f0f0f0"),
        strip.background=element_rect(fill="#d9d9d9")
    )


lat <- read_csv("latency-py312.csv")

ggplot(subset(lat, interrupted),
       aes(x=interval * 1e3, y=latency * 1e3, colour=impl)) +
    facet_wrap(~size) +
    geom_point()
