suppressPackageStartupMessages({
    library(tidyverse)
    library(broom)
    library(cowplot)
})

rt <- (
    read_csv("runtime-gil-py312.csv")
    |> separate_wider_delim(impl, "-", names=c("impl", "gil"))
    |> mutate(
           # explicit levels to control choice of contrasts in lm() below
           gil = factor(ifelse(gil=="nogil", "released", "held"),
                        levels=c("held", "released")),
           impl = factor(impl, levels=c("none", "simple", "coarse", "fine")),
           interval = interval * 1e3,
           elapsed = elapsed * 1e3,
           size.oddp2 = log2(size) %% 2 == 1
       )
)

## just the cost of check-whenever-possible
cwp <- (
    rt
    |> subset(impl %in% c("none", "simple"))
    |> mutate(mode = ordered(case_when(
           impl == "none"   & gil == "released"
                ~ "No checks, GIL released",
           impl == "none"   & gil == "held"
                ~ "No checks, GIL held",
           impl == "simple" & gil == "released"
                ~ "Check whenever possible, GIL released",
           impl == "simple" & gil == "held"
                ~ "Check whenever possible, GIL held",
       ), levels=c(
              "No checks, GIL held",
              "No checks, GIL released",
              "Check whenever possible, GIL held",
              "Check whenever possible, GIL released"
       )))
)

cwp.plot <- ggplot(cwp, aes(x=size, y=elapsed, colour=mode, shape=mode)) +
    geom_point(size=3) +
    geom_smooth(method="lm", formula=y~x, se=FALSE) +
    scale_x_continuous(
        "Number of samples",
        transform="log2",
        breaks=unique(rt$size),
        labels=scales::label_log(base=2),
        expand=expansion(mult=0.01, add=0)
    ) +
    scale_y_continuous(
        "Elapsed time (milliseconds)",
        transform="log10",
        limits=c(1, 2500),
        breaks=c(1, 10, 100, 1000, 2500),
        expand=expansion(mult=0.01, add=0)
    ) +
    scale_colour_manual(values=c(
        "No checks, GIL held"                   = "#e78ac3",
        "No checks, GIL released"               = "#fc8d62",
        "Check whenever possible, GIL held"     = "#66c2a5",
        "Check whenever possible, GIL released" = "#8da0cb"
    )) +
    scale_shape_manual(values=c(
        "No checks, GIL held"                   = 13,
        "No checks, GIL released"               =  9,
        "Check whenever possible, GIL held"     =  3,
        "Check whenever possible, GIL released" =  2
    )) +
    theme_half_open(font_family="Linux Biolinum O") +
    theme(legend.position="inside",
          legend.position.inside=c(.99, .02),
          legend.justification.inside=c(1, 0),
          legend.title=element_blank())

ggsave2("cost-check-whenever-possible-gil.svg",
        plot=cwp.plot, width=8, height=6, units="in", dpi=300)

costper <- (
    rt
    |> subset(interval <= 1)
    |> mutate(
        ns.per.sample = elapsed * 1e6 / size,
        disc.log.size = ordered(log2(size)),
        gil = ifelse(gil == "held", "GIL held", "GIL released"),
        impl = fct_recode(
            fct_relevel(impl, "fine", "coarse", "simple", "none"),
            "No checks" = "none",
            "Check whenever\npossible" = "simple",
            "Check every 1ms" = "fine",
            "Check every 1ms\n(coarse clock)" = "coarse"
        )
    )
)

cps.plot <-
    ggplot(costper, aes(y=impl, x=ns.per.sample, colour=disc.log.size)) +
    facet_wrap(~gil, ncol=1,
               strip.position="right") +
    geom_jitter(alpha=0.75, size=1) +
    scale_x_continuous("Nanoseconds per sample",
                       limits=c(0, 300),
                       expand=expansion(mult=0, add=0)) +
    scale_colour_manual(
        "# of samples",
        values=c(
            "16" = "#4a1486",
            "17" = "#8c2d04",
            "18" = "#6a51a3",
            "19" = "#d94801",
            "20" = "#807dba",
            "21" = "#f16913",
            "22" = "#9e9ac8",
            "23" = "#fd8dc3"
        ),
        labels = expression(
            2^16, 2^17, 2^18, 2^19,
            2^20, 2^21, 2^22, 2^23,
        )
    ) +
    theme_half_open(font_family="Linux Biolinum O") +
    background_grid(major="x", minor="x") +
    theme(legend.position="top",
          axis.title.y=element_blank(),
          strip.text=element_text(angle=0),
          plot.margin=margin(r=0.66,unit="in"))

ggsave2("cost-per-sample.svg",
        plot=cps.plot, width=11, height=5.5, units="in", dpi=300)

rti <- read_csv("runtime-intervals-py312.csv")
rtN <- rti |> subset(impl=="none")   |> select(size|rep|elapsed)
rtC <- rti |> subset(impl=="coarse") |> select(size|rep|interval|elapsed)
rtR <- (
    inner_join(rtC, rtN, by=c('size','rep'), suffix=c('.coarse', '.none'))
    |> mutate(interval = interval * 1e3,
              elapsed.coarse = elapsed.coarse * 1e3,
              elapsed.none = elapsed.none * 1e3,
              size.f = factor(size))
    |> group_by(size.f, interval)
    |> mutate(elapsed.coarse=sort(elapsed.coarse),
              elapsed.none=sort(elapsed.none),
              elapsed.ratio=elapsed.coarse/elapsed.none)
)

rtM <- summarise(rtR, .groups="drop",
                 elapsed.ratio=median(elapsed.ratio))

rtR <- ungroup(rtR)

etr.plot <- ggplot(rtR, aes(x=interval, y=elapsed.ratio, colour=size.f, shape=size.f)) +
    geom_jitter(width=0.25) +
    scale_colour_brewer(
        "# of samples",
        palette="Set2",
        labels = expression(2^17, 2^18, 2^19, 2^20, 2^21, 2^22)
    ) +
    scale_shape_discrete(
        "# of samples",
        labels = expression(2^17, 2^18, 2^19, 2^20, 2^21, 2^22)
    ) +
    scale_x_continuous(
        "Check interval (ms)",
        limits=c(0, 51),
        breaks=c(1, 10, 20, 30, 40, 50),
        expand=c(0, 0)
    ) +
    scale_y_continuous(
        "Elapsed time ratio (checks/no checks)",
        limits=c(0.4, 1.6),
        breaks=c(0.4, 0.6, 0.8, 1.0, 1.2, 1.4, 1.6)
    ) +
    theme_half_open(font_family="Linux Biolinum O") +
    background_grid(major="y") +
    theme(legend.position="top")

ggsave2("cost-by-interval.svg",
        plot=etr.plot, width=5.5, height=5.5, units="in", dpi=300)

etr.med.plot <-
    ggplot(rtM, aes(x=interval, y=elapsed.ratio, colour=size.f, shape=size.f)) +
    geom_point() +
    scale_colour_brewer(
        "# of samples",
        palette="Set2",
        labels = expression(2^17, 2^18, 2^19, 2^20, 2^21, 2^22),
        guide = guide_legend(nrow=1)
    ) +
    scale_shape_discrete(
        "# of samples",
        labels = expression(2^17, 2^18, 2^19, 2^20, 2^21, 2^22),
        guide = guide_legend(nrow=1)
    ) +
    scale_x_continuous(
        "Check interval (ms)",
        limits=c(0, 51),
        breaks=c(1, 10, 20, 30, 40, 50),
        expand=c(0, 0)
    ) +
    scale_y_continuous(
        "Median elapsed time ratio\n(coarse checks/no checks, GIL released)",
        limits=c(1.0, 1.6),
        expand=c(0, 0)
    ) +
    theme_half_open(font_family="Linux Biolinum O") +
    background_grid(major="y") +
    theme(legend.position="top")

ggsave2("cost-by-interval-median.svg",
        plot=etr.med.plot, width=5, height=4, units="in", dpi=300)
