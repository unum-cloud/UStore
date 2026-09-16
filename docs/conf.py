# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

from pathlib import Path

# Project information

project = "Unum · UStore"
html_title = "UStore: GPU-Accelerated Embedded Transactional Database for High-Throughput NVMe Devices"
copyright = "2023, Unum"
author = "Unum"
release = Path("../VERSION").read_text().strip()
version = release

# General configuration

extensions = [
    "breathe",
    "m2r2",
    "sphinx.ext.autodoc",
    "sphinx.ext.autosummary",
    "sphinx.ext.intersphinx",
    "sphinx.ext.napoleon",
    "sphinxcontrib.jquery",
    "sphinxcontrib.googleanalytics",
]

exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "*.md"]

googleanalytics_id = "341385789"
googleanalytics_enabled = True

# Options for HTML output

html_logo = "../assets/unum.png"
html_theme = "furo"
html_static_path = ["_static"]
html_css_files = ["custom.css"]
html_js_files = ["custom.js"]

breathe_projects = {"UStore": "../build/xml"}
breathe_default_project = "UStore"
