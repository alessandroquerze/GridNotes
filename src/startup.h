#pragma once

// Abilita o disabilita l'avvio automatico tramite Task Scheduler
// true  -> crea il task
// false -> rimuove il task
// ritorna true se successo
bool SetStartup(bool enabled);