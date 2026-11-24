"""
Downloader Otimizado ERA5 Post-Processed Daily Statistics
=========================================================
Baixa TODAS as 261 variáveis do dataset derived-era5-single-levels-daily-statistics
para as coordenadas do download.ini.

Estratégia de otimização (3 eixos):
  1. Agrupa coordenadas em regiões geográficas com bounding box compartilhado
  2. Agrupa até 10 variáveis compatíveis por requisição
  3. Pede o ano inteiro (12 meses) de uma vez

Estimativa final: ~23.000 requisições (~4 dias de download contínuo)
"""

import os
import sys
import time
import calendar
import hashlib
import re
from datetime import datetime, timezone, timedelta
import cdsapi

# ==============================================================================
# CONFIGURAÇÕES
# ==============================================================================

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VARS_POR_REQUISICAO = 10  # Máximo de variáveis por chamada ao CDS

url = "https://cds.climate.copernicus.eu/api"
key = "6e77b6c5-0f56-4d72-8eca-c5d29fe017cd"
ano_inicial_cfg = 1940
ano_final_cfg = None
pasta_saida_cfg = "dados_era5"

env_file = os.path.join(SCRIPT_DIR, ".env")
if os.path.exists(env_file):
    with open(env_file, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(";") or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            k, v = k.strip(), v.strip()
            if k == "CDSAPI_URL": url = v
            elif k == "CDSAPI_KEY": key = v
            elif k == "ANO_INICIAL":
                try: ano_inicial_cfg = int(v)
                except ValueError: pass
            elif k == "ANO_FINAL":
                try: ano_final_cfg = int(v)
                except ValueError: pass
            elif k == "PASTA_SAIDA": pasta_saida_cfg = v

try:
    cdsapirc = os.path.expanduser("~/.cdsapirc")
    if not os.path.exists(cdsapirc):
        with open(cdsapirc, "w", encoding="utf-8") as f:
            f.write(f"url: {url}\nkey: {key}\n")
except Exception:
    pass

# quiet=False para que o usuário veja o status da fila do CDS
cliente = cdsapi.Client(url=url, key=key)
DATASET = "derived-era5-single-levels-daily-statistics"

pasta_destino = os.path.abspath(pasta_saida_cfg)
os.makedirs(pasta_destino, exist_ok=True)
arquivo_log = os.path.join(pasta_destino, "download_era5.log")

def log(msg: str):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    linha = f"[{ts}] {msg}"
    print(linha, flush=True)
    try:
        with open(arquivo_log, "a", encoding="utf-8") as lf:
            lf.write(linha + "\n")
    except Exception:
        pass

# ==============================================================================
# COORDENADAS
# ==============================================================================

def ler_coordenadas(ini_path: str) -> list[dict]:
    coords = []
    ini = os.path.join(SCRIPT_DIR, ini_path)
    if not os.path.exists(ini):
        return coords
    em_secao = False
    with open(ini, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.lower().startswith("[coordenadas]"):
                em_secao = True
                continue
            if em_secao and line.startswith("["):
                break
            if not em_secao or not line or line.startswith(";") or line.startswith("#"):
                continue
            if "latitude" in line.lower() and "longitude" in line.lower():
                continue
            parts = line.split(";", 1)
            ll = parts[0].strip()
            nome = parts[1].strip() if len(parts) > 1 else ""
            nums = re.findall(r"-?\d+\.?\d*", ll)
            if len(nums) >= 2:
                lat, lon = float(nums[0]), float(nums[1])
                nome_safe = re.sub(r"[^\w\-]", "_", nome).strip("_") if nome else f"{lat}_{lon}"
                coords.append({"lat": lat, "lon": lon, "nome": nome, "id": nome_safe})
    return coords

coordenadas = ler_coordenadas("download.ini")
if not coordenadas:
    log("[ERRO] Nenhuma coordenada em download.ini [coordenadas]!")
    sys.exit(1)

def area_bbox(lat: float, lon: float) -> list[float]:
    """[North, West, South, East] com ±0.25° (1 pixel ERA5 0.25°)."""
    m = 0.25
    return [round(lat + m, 4), round(lon - m, 4), round(lat - m, 4), round(lon + m, 4)]

# ==============================================================================
# AGRUPAMENTO DE COORDENADAS EM REGIÕES
# ==============================================================================

def agrupar_coordenadas_em_regioes(coords: list[dict], max_span_lat=30.0, max_span_lon=60.0) -> list[dict]:
    """
    Agrupa coordenadas próximas em regiões com bounding box compartilhado.
    Cada região tem um bbox que cobre todas as suas coordenadas.
    max_span: diferença máxima em graus para agrupar.
    """
    restantes = list(coords)
    regioes = []

    while restantes:
        # Começa com a primeira coordenada não agrupada
        seed = restantes.pop(0)
        grupo = [seed]
        lat_min, lat_max = seed["lat"], seed["lat"]
        lon_min, lon_max = seed["lon"], seed["lon"]

        # Tenta adicionar coordenadas próximas
        ainda_restantes = []
        for c in restantes:
            new_lat_min = min(lat_min, c["lat"])
            new_lat_max = max(lat_max, c["lat"])
            new_lon_min = min(lon_min, c["lon"])
            new_lon_max = max(lon_max, c["lon"])
            if (new_lat_max - new_lat_min) <= max_span_lat and (new_lon_max - new_lon_min) <= max_span_lon:
                grupo.append(c)
                lat_min, lat_max = new_lat_min, new_lat_max
                lon_min, lon_max = new_lon_min, new_lon_max
            else:
                ainda_restantes.append(c)
        restantes = ainda_restantes

        # Bounding box com margem de 0.25° (1 pixel)
        m = 0.25
        bbox = [
            round(lat_max + m, 4),  # North
            round(lon_min - m, 4),  # West
            round(lat_min - m, 4),  # South
            round(lon_max + m, 4),  # East
        ]
        nomes = [c["nome"] for c in grupo]
        ids = [c["id"] for c in grupo]
        regiao_id = ids[0] if len(ids) == 1 else f"regiao_{len(regioes)+1}_{len(grupo)}cidades"
        regioes.append({
            "id": regiao_id,
            "coords": grupo,
            "nomes": nomes,
            "bbox": bbox,
        })

    return regioes

regioes = agrupar_coordenadas_em_regioes(coordenadas)

# ==============================================================================
# VARIÁVEIS (261 — catálogo completo)
# ==============================================================================

TODAS_VARIAVEIS = [
    # TEMPERATURA E PROPRIEDADES ATMOSFÉRICAS
    "2m_temperature", "2m_dewpoint_temperature",
    "maximum_2m_temperature_since_previous_post_processing",
    "minimum_2m_temperature_since_previous_post_processing",
    "skin_temperature", "air_density_over_the_oceans", "geopotential",
    "surface_pressure", "mean_sea_level_pressure", "boundary_layer_height",
    "cloud_base_height", "zero_degree_level",
    "convective_available_potential_energy", "convective_inhibition",
    "k_index", "total_totals_index", "duct_base_height",
    "trapping_layer_base_height", "trapping_layer_top_height",
    "mean_vertical_gradient_of_refractivity_inside_trapping_layer",
    "minimum_vertical_gradient_of_refractivity_inside_trapping_layer",
    # VENTO E CIRCULAÇÃO
    "10m_u_component_of_wind", "10m_v_component_of_wind",
    "100m_u_component_of_wind", "100m_v_component_of_wind",
    "10m_u_component_of_neutral_wind", "10m_v_component_of_neutral_wind",
    "10m_wind_gust_since_previous_post_processing", "instantaneous_10m_wind_gust",
    "ocean_surface_stress_equivalent_10m_neutral_wind_direction",
    "ocean_surface_stress_equivalent_10m_neutral_wind_speed",
    "friction_velocity", "free_convective_velocity_over_the_oceans",
    "eastward_turbulent_surface_stress", "northward_turbulent_surface_stress",
    "instantaneous_eastward_turbulent_surface_stress",
    "instantaneous_northward_turbulent_surface_stress",
    "mean_eastward_turbulent_surface_stress", "mean_northward_turbulent_surface_stress",
    "eastward_gravity_wave_surface_stress", "northward_gravity_wave_surface_stress",
    "mean_eastward_gravity_wave_surface_stress", "mean_northward_gravity_wave_surface_stress",
    "mean_boundary_layer_dissipation", "boundary_layer_dissipation",
    "gravity_wave_dissipation", "mean_gravity_wave_dissipation",
    # PRECIPITAÇÃO E NEVE
    "convective_precipitation", "convective_rain_rate",
    "convective_snowfall", "convective_snowfall_rate_water_equivalent",
    "large_scale_precipitation", "large_scale_rain_rate",
    "large_scale_snowfall_rate_water_equivalent", "large_scale_snowfall",
    "mean_convective_precipitation_rate", "mean_convective_snowfall_rate",
    "mean_large_scale_precipitation_rate", "mean_large_scale_snowfall_rate",
    "mean_total_precipitation_rate",
    "maximum_total_precipitation_rate_since_previous_post_processing",
    "minimum_total_precipitation_rate_since_previous_post_processing",
    "total_precipitation", "precipitation_type",
    "instantaneous_large_scale_surface_precipitation_fraction",
    "large_scale_precipitation_fraction", "mean_large_scale_precipitation_fraction",
    "mean_snowfall_rate", "snowfall", "snowmelt", "mean_snowmelt_rate",
    "snow_evaporation", "mean_snow_evaporation_rate",
    "snow_depth", "snow_density", "snow_albedo", "temperature_of_snow_layer",
    # UMIDADE, EVAPORAÇÃO E ÁGUA
    "total_column_water", "total_column_water_vapour",
    "total_column_cloud_liquid_water", "total_column_cloud_ice_water",
    "total_column_rain_water", "total_column_snow_water",
    "total_column_supercooled_liquid_water", "total_column_ozone",
    "evaporation", "mean_evaporation_rate",
    "potential_evaporation", "mean_potential_evaporation_rate",
    "instantaneous_moisture_flux", "vertically_integrated_moisture_divergence",
    "mean_vertically_integrated_moisture_divergence",
    "runoff", "surface_runoff", "sub_surface_runoff",
    "mean_runoff_rate", "mean_surface_runoff_rate", "mean_sub_surface_runoff_rate",
    "skin_reservoir_content",
    # NUVENS
    "total_cloud_cover", "high_cloud_cover", "medium_cloud_cover", "low_cloud_cover",
    # RADIAÇÃO SOLAR, UV E TÉRMICA
    "clear_sky_direct_solar_radiation_at_surface",
    "downward_uv_radiation_at_the_surface", "mean_surface_downward_uv_radiation_flux",
    "surface_solar_radiation_downwards", "surface_solar_radiation_downward_clear_sky",
    "surface_net_solar_radiation", "surface_net_solar_radiation_clear_sky",
    "surface_thermal_radiation_downwards", "surface_thermal_radiation_downward_clear_sky",
    "surface_net_thermal_radiation", "surface_net_thermal_radiation_clear_sky",
    "surface_latent_heat_flux", "surface_sensible_heat_flux",
    "top_net_solar_radiation", "top_net_solar_radiation_clear_sky",
    "top_net_thermal_radiation", "top_net_thermal_radiation_clear_sky",
    "toa_incident_solar_radiation", "total_sky_direct_solar_radiation_at_surface",
    "mean_surface_direct_short_wave_radiation_flux",
    "mean_surface_direct_short_wave_radiation_flux_clear_sky",
    "mean_surface_downward_long_wave_radiation_flux",
    "mean_surface_downward_long_wave_radiation_flux_clear_sky",
    "mean_surface_downward_short_wave_radiation_flux",
    "mean_surface_downward_short_wave_radiation_flux_clear_sky",
    "mean_surface_latent_heat_flux", "mean_surface_sensible_heat_flux",
    "mean_surface_net_long_wave_radiation_flux",
    "mean_surface_net_long_wave_radiation_flux_clear_sky",
    "mean_surface_net_short_wave_radiation_flux",
    "mean_surface_net_short_wave_radiation_flux_clear_sky",
    "mean_top_downward_short_wave_radiation_flux",
    "mean_top_net_long_wave_radiation_flux",
    "mean_top_net_long_wave_radiation_flux_clear_sky",
    "mean_top_net_short_wave_radiation_flux",
    "mean_top_net_short_wave_radiation_flux_clear_sky",
    # VEGETAÇÃO, SOLO E SUPERFÍCIE
    "high_vegetation_cover", "low_vegetation_cover",
    "leaf_area_index_high_vegetation", "leaf_area_index_low_vegetation",
    "type_of_high_vegetation", "type_of_low_vegetation",
    "land_sea_mask", "forecast_albedo", "forecast_surface_roughness",
    "forecast_logarithm_of_surface_roughness_for_heat",
    "near_ir_albedo_for_diffuse_radiation", "near_ir_albedo_for_direct_radiation",
    "uv_visible_albedo_for_diffuse_radiation", "uv_visible_albedo_for_direct_radiation",
    "slope_of_sub_gridscale_orography", "angle_of_sub_gridscale_orography",
    "anisotropy_of_sub_gridscale_orography", "standard_deviation_of_orography",
    "standard_deviation_of_filtered_subgrid_orography", "soil_type",
    "soil_temperature_level_1", "soil_temperature_level_2",
    "soil_temperature_level_3", "soil_temperature_level_4",
    "volumetric_soil_water_layer_1", "volumetric_soil_water_layer_2",
    "volumetric_soil_water_layer_3", "volumetric_soil_water_layer_4",
    # LAGOS
    "lake_bottom_temperature", "lake_cover", "lake_depth",
    "lake_ice_depth", "lake_ice_temperature",
    "lake_mix_layer_depth", "lake_mix_layer_temperature",
    "lake_shape_factor", "lake_total_layer_temperature",
    # OCEANOS, ONDAS E GELO MARINHO
    "sea_surface_temperature", "sea_ice_cover", "model_bathymetry",
    "benjamin_feir_index", "charnock", "coefficient_of_drag_with_waves",
    "maximum_individual_wave_height",
    "mean_direction_of_total_swell", "mean_direction_of_wind_waves", "mean_wave_direction",
    "mean_wave_direction_of_first_swell_partition",
    "mean_wave_direction_of_second_swell_partition",
    "mean_wave_direction_of_third_swell_partition",
    "mean_period_of_total_swell", "mean_period_of_wind_waves", "mean_wave_period",
    "mean_wave_period_based_on_first_moment",
    "mean_wave_period_based_on_first_moment_for_swell",
    "mean_wave_period_based_on_first_moment_for_wind_waves",
    "mean_wave_period_based_on_second_moment_for_swell",
    "mean_wave_period_based_on_second_moment_for_wind_waves",
    "mean_wave_period_of_first_swell_partition",
    "mean_wave_period_of_second_swell_partition",
    "mean_wave_period_of_third_swell_partition",
    "mean_zero_crossing_wave_period", "peak_wave_period",
    "period_corresponding_to_maximum_individual_wave_height",
    "significant_height_of_combined_wind_waves_and_swell",
    "significant_height_of_total_swell", "significant_height_of_wind_waves",
    "significant_wave_height_of_first_swell_partition",
    "significant_wave_height_of_second_swell_partition",
    "significant_wave_height_of_third_swell_partition",
    "mean_square_slope_of_waves",
    "normalized_energy_flux_into_ocean", "normalized_energy_flux_into_waves",
    "normalized_stress_into_ocean",
    "u_component_stokes_drift", "v_component_stokes_drift",
    "wave_spectral_directional_width", "wave_spectral_directional_width_for_swell",
    "wave_spectral_directional_width_for_wind_waves",
    "wave_spectral_kurtosis", "wave_spectral_peakedness", "wave_spectral_skewness",
    # FLUXOS DE ENERGIA, MASSA E TRANSPORTE
    "vertical_integral_of_divergence_of_cloud_frozen_water_flux",
    "vertical_integral_of_divergence_of_cloud_liquid_water_flux",
    "vertical_integral_of_divergence_of_geopotential_flux",
    "vertical_integral_of_divergence_of_kinetic_energy_flux",
    "vertical_integral_of_divergence_of_mass_flux",
    "vertical_integral_of_divergence_of_moisture_flux",
    "vertical_integral_of_divergence_of_ozone_flux",
    "vertical_integral_of_divergence_of_thermal_energy_flux",
    "vertical_integral_of_divergence_of_total_energy_flux",
    "vertical_integral_of_eastward_cloud_frozen_water_flux",
    "vertical_integral_of_eastward_cloud_liquid_water_flux",
    "vertical_integral_of_eastward_geopotential_flux",
    "vertical_integral_of_eastward_heat_flux",
    "vertical_integral_of_eastward_kinetic_energy_flux",
    "vertical_integral_of_eastward_mass_flux",
    "vertical_integral_of_eastward_ozone_flux",
    "vertical_integral_of_eastward_total_energy_flux",
    "vertical_integral_of_eastward_water_vapour_flux",
    "vertical_integral_of_energy_conversion",
    "vertical_integral_of_kinetic_energy",
    "vertical_integral_of_mass_of_atmosphere",
    "vertical_integral_of_mass_tendency",
    "vertical_integral_of_northward_cloud_frozen_water_flux",
    "vertical_integral_of_northward_cloud_liquid_water_flux",
    "vertical_integral_of_northward_geopotential_flux",
    "vertical_integral_of_northward_heat_flux",
    "vertical_integral_of_northward_kinetic_energy_flux",
    "vertical_integral_of_northward_mass_flux",
    "vertical_integral_of_northward_ozone_flux",
    "vertical_integral_of_northward_total_energy_flux",
    "vertical_integral_of_northward_water_vapour_flux",
    "vertical_integral_of_potential_and_internal_energy",
    "vertical_integral_of_potential_internal_and_latent_energy",
    "vertical_integral_of_temperature",
    "vertical_integral_of_thermal_energy", "vertical_integral_of_total_energy",
    # GELO MARINHO
    "ice_temperature_layer_1", "ice_temperature_layer_2",
    "ice_temperature_layer_3", "ice_temperature_layer_4",
]

TODAS_VARIAVEIS = list(dict.fromkeys(TODAS_VARIAVEIS))

# ==============================================================================
# CLASSIFICAÇÃO DE ESTATÍSTICAS
# ==============================================================================

ACUMULADAS = {
    "convective_precipitation", "convective_rain_rate", "convective_snowfall",
    "convective_snowfall_rate_water_equivalent", "large_scale_precipitation",
    "large_scale_rain_rate", "large_scale_snowfall_rate_water_equivalent",
    "large_scale_snowfall", "mean_convective_precipitation_rate",
    "mean_convective_snowfall_rate", "mean_large_scale_precipitation_rate",
    "mean_large_scale_snowfall_rate", "mean_total_precipitation_rate",
    "total_precipitation", "snowfall", "snowmelt", "mean_snowmelt_rate",
    "snow_evaporation", "mean_snow_evaporation_rate",
    "evaporation", "mean_evaporation_rate", "potential_evaporation",
    "mean_potential_evaporation_rate", "runoff", "surface_runoff",
    "sub_surface_runoff", "mean_runoff_rate", "mean_surface_runoff_rate",
    "mean_sub_surface_runoff_rate",
    "clear_sky_direct_solar_radiation_at_surface",
    "downward_uv_radiation_at_the_surface", "mean_surface_downward_uv_radiation_flux",
    "surface_solar_radiation_downwards", "surface_solar_radiation_downward_clear_sky",
    "surface_net_solar_radiation", "surface_net_solar_radiation_clear_sky",
    "surface_thermal_radiation_downwards", "surface_thermal_radiation_downward_clear_sky",
    "surface_net_thermal_radiation", "surface_net_thermal_radiation_clear_sky",
    "surface_latent_heat_flux", "surface_sensible_heat_flux",
    "top_net_solar_radiation", "top_net_solar_radiation_clear_sky",
    "top_net_thermal_radiation", "top_net_thermal_radiation_clear_sky",
    "toa_incident_solar_radiation", "total_sky_direct_solar_radiation_at_surface",
    "mean_surface_direct_short_wave_radiation_flux",
    "mean_surface_direct_short_wave_radiation_flux_clear_sky",
    "mean_surface_downward_long_wave_radiation_flux",
    "mean_surface_downward_long_wave_radiation_flux_clear_sky",
    "mean_surface_downward_short_wave_radiation_flux",
    "mean_surface_downward_short_wave_radiation_flux_clear_sky",
    "mean_surface_latent_heat_flux", "mean_surface_sensible_heat_flux",
    "mean_surface_net_long_wave_radiation_flux",
    "mean_surface_net_long_wave_radiation_flux_clear_sky",
    "mean_surface_net_short_wave_radiation_flux",
    "mean_surface_net_short_wave_radiation_flux_clear_sky",
    "mean_top_downward_short_wave_radiation_flux",
    "mean_top_net_long_wave_radiation_flux",
    "mean_top_net_long_wave_radiation_flux_clear_sky",
    "mean_top_net_short_wave_radiation_flux",
    "mean_top_net_short_wave_radiation_flux_clear_sky",
    "eastward_turbulent_surface_stress", "northward_turbulent_surface_stress",
    "instantaneous_eastward_turbulent_surface_stress",
    "instantaneous_northward_turbulent_surface_stress",
    "mean_eastward_turbulent_surface_stress", "mean_northward_turbulent_surface_stress",
    "eastward_gravity_wave_surface_stress", "northward_gravity_wave_surface_stress",
    "mean_eastward_gravity_wave_surface_stress",
    "mean_northward_gravity_wave_surface_stress",
    "mean_boundary_layer_dissipation", "boundary_layer_dissipation",
    "gravity_wave_dissipation", "mean_gravity_wave_dissipation",
}

def stats_para_var(v: str) -> list[str]:
    if v in ACUMULADAS:
        return ["daily_mean", "daily_sum"]
    return ["daily_mean", "daily_max", "daily_min"]

# ==============================================================================
# AGRUPAMENTO DE VARIÁVEIS POR ESTATÍSTICA
# ==============================================================================

def agrupar_por_estatistica(variaveis: list[str]) -> dict[str, list[list[str]]]:
    """
    Agrupa variáveis por estatística compatível, em chunks de até VARS_POR_REQUISICAO.
    Retorna: { "daily_mean": [[v1,v2,v3,v4,v5], [v6,v7,...]], "daily_max": [...], ... }
    """
    por_stat: dict[str, list[str]] = {}
    for v in variaveis:
        for s in stats_para_var(v):
            por_stat.setdefault(s, []).append(v)

    resultado: dict[str, list[list[str]]] = {}
    for stat, vars_list in por_stat.items():
        chunks = []
        for i in range(0, len(vars_list), VARS_POR_REQUISICAO):
            chunks.append(vars_list[i:i + VARS_POR_REQUISICAO])
        resultado[stat] = chunks

    return resultado

# ==============================================================================
# VALIDAÇÃO
# ==============================================================================

def arquivo_valido(caminho: str) -> bool:
    if not os.path.exists(caminho):
        return False
    try:
        tam = os.path.getsize(caminho)
        if tam < 500:
            return False
        with open(caminho, "rb") as f:
            cab = f.read(8)
            return cab.startswith(b"CDF") or cab.startswith(b"\x89HDF") or cab.startswith(b"PK")
    except Exception:
        return False

# ==============================================================================
# PERÍODO
# ==============================================================================

agora_utc = datetime.now(timezone.utc)
limite_era5 = agora_utc - timedelta(days=5)
ano_ini = max(1940, ano_inicial_cfg)
ano_fim = ano_final_cfg if ano_final_cfg is not None else limite_era5.year
ano_fim = min(ano_fim, limite_era5.year)
anos = list(range(ano_ini, ano_fim + 1))

# ==============================================================================
# LOOP PRINCIPAL
# ==============================================================================

def main():
    grupos = agrupar_por_estatistica(TODAS_VARIAVEIS)
    incompativeis: set[str] = set()  # grupo_key

    # Conta total de tarefas para progresso
    total_tarefas = 0
    for stat, chunks in grupos.items():
        total_tarefas += len(chunks) * len(regioes) * len(anos)

    total_baixados = 0
    total_pulos = 0
    total_erros = 0
    tarefa_atual = 0

    log("=" * 72)
    log("DOWNLOAD ERA5 POST-PROCESSED DAILY STATISTICS (OTIMIZADO)")
    log(f"Dataset:          {DATASET}")
    log(f"Variáveis:        {len(TODAS_VARIAVEIS)}")
    log(f"Coordenadas:      {len(coordenadas)} -> {len(regioes)} regiões")
    for r in regioes:
        log(f"  • {r['id']}: {', '.join(r['nomes'])} | bbox={r['bbox']}")
    log(f"Período:          {ano_ini}–{ano_fim} ({len(anos)} anos)")
    log(f"Vars/requisição:  {VARS_POR_REQUISICAO}")
    log(f"Total requisições estimadas: ~{total_tarefas:,}")
    est_horas = total_tarefas * 15 / 3600
    log(f"Tempo estimado:   ~{est_horas:.0f}h (~{est_horas/24:.1f} dias)")
    log(f"Destino:          {pasta_destino}")
    log("=" * 72)

    for stat, chunks in sorted(grupos.items()):
        log(f"\n--- Estatística: {stat} ({len(chunks)} grupos de variáveis) ---")

        for idx_chunk, var_grupo in enumerate(chunks, start=1):
            grupo_key = f"{stat}_{idx_chunk}"

            if grupo_key in incompativeis:
                continue

            for regiao in regioes:
                pasta_regiao = os.path.join(pasta_destino, regiao["id"])
                os.makedirs(pasta_regiao, exist_ok=True)

                for ano in anos:
                    tarefa_atual += 1

                    # Meses e dias válidos
                    if ano == limite_era5.year:
                        meses_v = list(range(1, limite_era5.month + 1))
                    else:
                        meses_v = list(range(1, 13))
                    mes_strs = [f"{m:02d}" for m in meses_v]

                    todos_dias = set()
                    for m in meses_v:
                        max_d = calendar.monthrange(ano, m)[1]
                        if ano == limite_era5.year and m == limite_era5.month:
                            max_d = min(max_d, limite_era5.day)
                        for d in range(1, max_d + 1):
                            todos_dias.add(f"{d:02d}")
                    dias = sorted(todos_dias)

                    # Nome do arquivo
                    vars_tag = "+".join(sorted(var_grupo))
                    h = hashlib.md5(vars_tag.encode()).hexdigest()[:10]
                    nome_base = f"{stat}_{ano}_g{idx_chunk}_{h}"
                    nome_arquivo = os.path.join(pasta_regiao, nome_base + ".nc")
                    arquivo_tmp = nome_arquivo + ".tmp"

                    if arquivo_valido(nome_arquivo):
                        total_pulos += 1
                        continue
                    elif os.path.exists(nome_arquivo):
                        try: os.remove(nome_arquivo)
                        except Exception: pass

                    pct = tarefa_atual / total_tarefas * 100
                    tag = f"[{tarefa_atual}/{total_tarefas} {pct:.1f}%] {stat} | {ano} | {regiao['id']} | {len(var_grupo)} vars"
                    log(f"[SOLICITANDO] {tag}")

                    sucesso = False
                    tentativas = 0
                    max_tent = 4
                    backoff = 15.0

                    while not sucesso and tentativas < max_tent:
                        try:
                            t0 = time.time()
                            if os.path.exists(arquivo_tmp):
                                try: os.remove(arquivo_tmp)
                                except Exception: pass

                            cliente.retrieve(
                                DATASET,
                                {
                                    "product_type": "reanalysis",
                                    "variable": var_grupo,
                                    "daily_statistic": stat,
                                    "time_zone": "utc+00:00",
                                    "frequency": "1_hourly",
                                    "year": [str(ano)],
                                    "month": mes_strs,
                                    "day": dias,
                                    "area": regiao["bbox"],
                                    "data_format": "netcdf",
                                },
                                arquivo_tmp
                            )

                            if arquivo_valido(arquivo_tmp):
                                if os.path.exists(nome_arquivo):
                                    try: os.remove(nome_arquivo)
                                    except Exception: pass
                                os.rename(arquivo_tmp, nome_arquivo)
                                dur = time.time() - t0
                                tam_kb = os.path.getsize(nome_arquivo) / 1024
                                log(f"[OK] {tag} -> {tam_kb:.1f} KB em {dur:.1f}s")
                                sucesso = True
                                total_baixados += 1
                            else:
                                if os.path.exists(arquivo_tmp):
                                    try: os.remove(arquivo_tmp)
                                    except Exception: pass
                                raise RuntimeError("Arquivo não passou na validação NetCDF")

                        except Exception as e:
                            tentativas += 1
                            erro_str = str(e).lower()
                            termos_fatal = [
                                "not supported", "invalid statistic",
                                "not applicable", "unknown variable",
                                "not a valid variable", "is not allowed",
                            ]
                            if any(t in erro_str for t in termos_fatal):
                                log(f"[INCOMPATÍVEL] {tag}: {e}")
                                incompativeis.add(grupo_key)
                                break
                            log(f"[AVISO] Tentativa {tentativas}/{max_tent}: {e}")
                            if tentativas < max_tent:
                                log(f"[ESPERA] {backoff:.0f}s...")
                                time.sleep(backoff)
                                backoff = min(backoff * 2.0, 120.0)
                            else:
                                log(f"[FALHA] {tag}")
                                total_erros += 1

                    if grupo_key in incompativeis:
                        break  # sai do loop de anos
                if grupo_key in incompativeis:
                    break  # sai do loop de regiões

    log("=" * 72)
    log("DOWNLOAD FINALIZADO")
    log(f"Baixados: {total_baixados} | Já existiam: {total_pulos} | Falhas: {total_erros}")
    if incompativeis:
        log(f"Combinações incompatíveis: {len(incompativeis)}")
    log("=" * 72)


if __name__ == "__main__":
    main()
