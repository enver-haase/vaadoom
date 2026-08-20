package org.vaadin.addons.enverhaase.vaadoom;

import com.vaadin.flow.component.html.H1;
import com.vaadin.flow.component.html.Paragraph;
import com.vaadin.flow.component.orderedlayout.VerticalLayout;
import com.vaadin.flow.router.BeforeEnterEvent;
import com.vaadin.flow.router.BeforeEnterObserver;
import com.vaadin.flow.router.Route;

/**
 * Local demo view for the Vaadoom add-on. Not shipped in the JAR (test scope).
 * Run with {@code mvn} and open http://localhost:8080/ to see the component.
 * <p>
 * A different IWAD can be tried without a restart via {@code ?wad=<url>}, e.g.
 * {@code http://localhost:8080/?wad=http://localhost:8099/DOOM2.WAD} — handy for
 * checking that a full game identifies and plays correctly.
 */
@Route("")
public class DemoView extends VerticalLayout implements BeforeEnterObserver {

    private final Vaadoom doom;

    public DemoView() {
        setSizeFull();
        setAlignItems(Alignment.CENTER);

        add(new H1("Vaadoom demo"));
        add(new Paragraph(
                "The viewport below is the Vaadoom Flow component. It boots a NOMMU Linux on a "
                        + "SUBLEQ VM compiled to WebAssembly and plays the WAD it is given "
                        + "(first paint takes ~15 seconds)."));
        add(new Paragraph(
                "This demo fetches id Software's freely redistributable shareware WAD from the "
                        + "Internet Archive. Own DOOM, The Ultimate DOOM, DOOM II or Final "
                        + "DOOM? Host that WAD yourself and pass its URL to play the full game."));

        // Any IWAD URL works here - see Vaadoom's javadoc. Cross-origin URLs must
        // send CORS headers; a WAD served by your own application never needs any.
        doom = new Vaadoom(Vaadoom.SHAREWARE_WAD);
        add(doom);
    }

    @Override
    public void beforeEnter(BeforeEnterEvent event) {
        event.getLocation().getQueryParameters().getSingleParameter("wad")
                .ifPresent(doom::setWadUrl);
    }
}
