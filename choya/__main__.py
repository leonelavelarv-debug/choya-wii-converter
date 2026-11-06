from .cli import main

# La protección es obligatoria en Windows: los procesos de conversión en paralelo
# vuelven a importar este módulo y, sin ella, relanzarían la conversión completa.
if __name__ == "__main__":
    main()
