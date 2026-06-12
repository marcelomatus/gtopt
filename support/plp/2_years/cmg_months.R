library(tidyverse)
library(arrow)
library(data.table)

PATH = "~/git/plp_colbun/test_cases/cen/20240611/original"
setwd(PATH)

fread("indhor.csv") %>%
  rename(
    year = 1,
    month = Mes,
    block = Bloque) %>%
  distinct(block, month, year) %>%
  mutate(date = ym(paste(year, month)))-> block_month

plpbar <- fread("plpbar.csv")

plpbar %>%
  filter(Hidro == "MEDIA", BarNom == "AJahuel220") %>%
  group_by(Hidro, block = Bloque) %>%
  summarise(CMg = sum(CMgBar * DemBarE) / sum(DemBarE)) %>%
  left_join(block_month, by = "block") %>%
  group_by(Hidro, date) %>%
  summarise(CMg = mean(CMg)) -> cmg_mean_date

cmg_mean_date %>%
  write_csv_arrow("TABLE_CMg_months.csv")

cmg_mean_date %>%
  ggplot(aes(x=date, y=CMg, color=Hidro)) +
  geom_line() +
  theme(legend.position = "none") +
  labs(
    x = "Year",
    y = "Mean Marginal Cost (US$/MWh)"
  ) +
  scale_y_continuous(limits = c(0, 100)) +
  #scale_x_continuous(breaks = 2024:2054) +
  theme_bw() +
  theme(
    axis.text.x = element_text(angle = 45)
  ) -> plt_cmg_mean_months

ggsave("plt_cmg_mean_month.png", plt_cmg_mean_months)
